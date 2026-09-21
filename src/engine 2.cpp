#include <kronos/engine.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kronos {

namespace {

std::filesystem::path prepareDatabaseDirectory(
    const std::filesystem::path &db_path) {
  if (db_path.empty()) {
    throw std::invalid_argument("Database path cannot be empty");
  }

  std::filesystem::create_directories(db_path);
  return db_path;
}

size_t memtableBytesFromConfig(const Config &config) {
  const int configured_mb = config.getMemtableSize();

  if (configured_mb <= 0) {
    throw std::invalid_argument("memtable_size_mb must be greater than zero");
  }

  constexpr size_t BYTES_PER_MB = 1024ULL * 1024ULL;
  const auto mb = static_cast<size_t>(configured_mb);

  if (mb > std::numeric_limits<size_t>::max() / BYTES_PER_MB) {
    throw std::overflow_error("Configured Memtable size is too large");
  }

  return mb * BYTES_PER_MB;
}

void considerNewer(std::optional<InternalEntry> &winner,
                   std::optional<InternalEntry> candidate) {
  if (!candidate.has_value()) {
    return;
  }

  if (!winner.has_value() || candidate->sequence > winner->sequence) {
    winner = std::move(candidate);
  }
}

} // namespace

KronosEngine::KronosEngine(const Config &config,
                           const std::filesystem::path &db_path)
    : db_path_(prepareDatabaseDirectory(db_path)),
      memtable_target_bytes_(memtableBytesFromConfig(config)),
      wal_(db_path_ / "WAL"),
      compaction_policy_(config.getL0CompactionTrigger()), compactor_(),
      active_memtable_(std::make_unique<Memtable>(memtable_target_bytes_)),
      immutable_memtable_(nullptr), manifest_(db_path_ / "MANIFEST"),
      flush_queue_(), write_mutex_(), state_mutex_(), manifest_mutex_(),
      immutable_cleared_cv_(), background_error_(nullptr),
      shutting_down_(false), background_worker_() {

  /*
   * Module 9 owns full WAL replay / crash recovery.
   *
   * Module 8 deliberately starts only the concurrency/maintenance machinery.
   * The worker is started here (rather than in the initializer list) so every
   * mutex, condition variable, queue and storage component it can touch is
   * already fully constructed.
   */
  background_worker_ =
      std::thread(&KronosEngine::backgroundWorkerLoop, this);
}

KronosEngine::~KronosEngine() {
  /*
   * Destructors must not let exceptions escape. Explicit callers of shutdown()
   * still receive maintenance failures; destruction performs the same cleanup
   * but suppresses any final rethrow.
   */
  try {
    shutdown();
  } catch (...) {
  }
}

void KronosEngine::rethrowBackgroundErrorLocked() const {
  if (background_error_) {
    std::rethrow_exception(background_error_);
  }
}

void KronosEngine::prepareForWriteLocked(
    std::unique_lock<std::mutex> &state_lock, const std::string &key,
    const InternalEntry &candidate) {

  if (!state_lock.owns_lock()) {
    throw std::logic_error("prepareForWriteLocked requires state_mutex_");
  }

  if (shutting_down_) {
    throw std::runtime_error("KronosEngine is shutting down");
  }

  rethrowBackgroundErrorLocked();

  /*
   * A single record may legitimately be larger than the configured Memtable
   * target. Rotating an empty Memtable would create an empty immutable table,
   * which cannot be flushed to a valid SSTable. Therefore size-triggered
   * rotation is meaningful only when the active Memtable already contains at
   * least one record.
   */
  auto rotation_required = [&] {
    return active_memtable_->entry_count() > 0 &&
           active_memtable_->would_exceed_target(key, candidate);
  };

  /*
   * Backpressure:
   *
   * If the active Memtable needs to rotate while the one immutable slot is
   * occupied, the foreground writer sleeps. condition_variable::wait releases
   * state_mutex_ while sleeping, allowing the background worker to commit the
   * immutable Memtable, clear the slot, and notify us.
   */
  while (rotation_required() && immutable_memtable_) {
    immutable_cleared_cv_.wait(state_lock, [this] {
      return !immutable_memtable_ || shutting_down_ || background_error_;
    });

    if (shutting_down_) {
      throw std::runtime_error("KronosEngine is shutting down");
    }

    rethrowBackgroundErrorLocked();
  }

  if (rotation_required()) {
    rotateActiveMemtableLocked();
  }
}

void KronosEngine::rotateActiveMemtableLocked() {
  if (!active_memtable_) {
    throw std::logic_error("No active Memtable to rotate");
  }

  if (immutable_memtable_) {
    throw std::logic_error("Immutable Memtable slot is already occupied");
  }

  if (active_memtable_->entry_count() == 0) {
    return;
  }

  if (!active_memtable_->freeze()) {
    throw std::logic_error("Active Memtable could not be frozen");
  }

  /*
   * Transfer ownership from the foreground's unique_ptr into shared lifetime.
   * Foreground GETs and the background flush worker may now read the same
   * frozen Memtable concurrently, but nobody may mutate it.
   */
  auto frozen = std::shared_ptr<Memtable>(std::move(active_memtable_));
  immutable_memtable_ = frozen;

  // The foreground immediately receives a fresh mutable Memtable.
  active_memtable_ = std::make_unique<Memtable>(memtable_target_bytes_);

  /*
   * Enqueue while state_mutex_ is still held. Engine shutdown takes the same
   * mutex before closing the queue, so a successful rotation cannot race with
   * queue shutdown and leave an immutable Memtable unscheduled.
   */
  if (!flush_queue_.push(std::move(frozen))) {
    throw std::runtime_error(
        "Background flush queue rejected a Memtable before engine shutdown");
  }
}

void KronosEngine::put(const std::string &key, const std::string &value) {
  /*
   * Serializing foreground writers preserves WAL sequence ordering without
   * requiring the WAL itself to become a multi-writer component in Module 8.
   * Reads do not take write_mutex_ and may proceed concurrently.
   */
  std::lock_guard<std::mutex> writer_lock(write_mutex_);

  // Sequence does not affect the size estimate. UINT64_MAX makes the candidate
  // unambiguously newer if this key already exists in the active Memtable.
  InternalEntry candidate{value, std::numeric_limits<uint64_t>::max(),
                          OperationType::PUT};

  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    prepareForWriteLocked(state_lock, key, candidate);
  }

  // WAL first: an acknowledged Memtable mutation must always have a durable
  // record preceding it.
  const uint64_t sequence = wal_.put(key, value);

  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    const auto result = active_memtable_->put(key, value, sequence);

    if (result != Memtable::WriteResult::SUCCESS) {
      throw std::logic_error("Active Memtable rejected a WAL-backed PUT");
    }
  }
}

void KronosEngine::remove(const std::string &key) {
  std::lock_guard<std::mutex> writer_lock(write_mutex_);

  InternalEntry candidate{"", std::numeric_limits<uint64_t>::max(),
                          OperationType::DELETE};

  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    prepareForWriteLocked(state_lock, key, candidate);
  }

  const uint64_t sequence = wal_.remove(key);

  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    const auto result = active_memtable_->remove(key, sequence);

    if (result != Memtable::WriteResult::SUCCESS) {
      throw std::logic_error("Active Memtable rejected a WAL-backed DELETE");
    }
  }
}

GetResult KronosEngine::get(const std::string &key) {
  std::optional<InternalEntry> winner;
  std::shared_ptr<Memtable> immutable_snapshot;

  /*
   * Keep the active-Memtable lookup inside state_mutex_ because foreground
   * writes mutate its std::map. Only copy the immutable shared_ptr here; the
   * immutable object itself can safely be read after releasing the mutex.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    considerNewer(winner, active_memtable_
                              ? active_memtable_->lookupEntry(key)
                              : std::optional<InternalEntry>{});

    immutable_snapshot = immutable_memtable_;
  }

  if (immutable_snapshot) {
    considerNewer(winner, immutable_snapshot->lookupEntry(key));
  }

  /*
   * Copy MANIFEST state while holding manifest_mutex_, then release the mutex
   * before opening/reading any files. The maintenance worker may commit a new
   * snapshot afterward, but Module 8 deliberately does not physically delete
   * obsolete SSTables, so this read snapshot remains safe to use.
   */
  std::vector<SstableMetadata> live_files;
  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    live_files = manifest_.liveFiles();
  }

  for (const auto &metadata : live_files) {
    // Key-range metadata gives us a cheap exact exclusion before opening the
    // SSTable and consulting its Bloom filter / sparse index.
    if (key < metadata.smallest_key || key > metadata.largest_key) {
      continue;
    }

    SstableReader reader(metadata.path);
    considerNewer(winner, reader.lookupEntry(key));
  }

  if (!winner.has_value()) {
    return {.value = "", .status = GetStatus::NOT_FOUND};
  }

  if (winner->operation == OperationType::DELETE) {
    return {.value = "", .status = GetStatus::DELETED};
  }

  return {.value = winner->value, .status = GetStatus::FOUND};
}

std::filesystem::path KronosEngine::allocateSstablePath(size_t level) {
  while (true) {
    const auto filename = "L" + std::to_string(level) + "-" +
                          std::to_string(next_sstable_id_++) + ".sst";
    const auto candidate = db_path_ / filename;

    /*
     * Physical files not present in MANIFEST may be crash leftovers/orphans.
     * Never overwrite them. Module 9 will own explicit orphan cleanup.
     */
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
}

SstableMetadata
KronosEngine::metadataForSstable(const std::filesystem::path &path,
                                 size_t level) const {
  SstableReader reader(path);
  auto iterator = reader.getIterator();

  if (!iterator.valid()) {
    throw std::runtime_error("Cannot create metadata for an empty SSTable");
  }

  std::string smallest_key = iterator.getKey();
  std::string largest_key = smallest_key;

  while (iterator.valid()) {
    largest_key = iterator.getKey();
    iterator.next();
  }

  return SstableMetadata{path, level, std::move(smallest_key),
                         std::move(largest_key)};
}

void KronosEngine::flushMemtable(const std::shared_ptr<Memtable> &memtable) {
  if (!memtable) {
    throw std::invalid_argument("Cannot flush a null Memtable");
  }

  if (memtable->GetState() != Memtable::MemTableState::IMMUTABLE) {
    throw std::logic_error("Background worker received a mutable Memtable");
  }

  if (memtable->entry_count() == 0) {
    throw std::logic_error("Background worker received an empty Memtable");
  }

  const auto output_path = allocateSstablePath(0);
  SstableBuilder builder(output_path, sstable_block_size_, bloom_bits_per_key_);

  auto iterator = memtable->getIterator();

  if (!iterator.valid()) {
    throw std::logic_error("Non-empty Memtable produced an empty iterator");
  }

  std::string smallest_key = iterator.key();
  std::string largest_key = smallest_key;

  while (iterator.valid()) {
    largest_key = iterator.key();
    builder.add(iterator.key(), iterator.entry());
    iterator.next();
  }

  // finish() finalizes the SSTable before MANIFEST is allowed to reference it.
  builder.finish();

  ManifestEdit edit;
  edit.add_files.push_back(
      SstableMetadata{output_path, 0, smallest_key, largest_key});

  /*
   * Logical truth first: the new L0 SSTable becomes part of the database only
   * after MANIFEST commits the edit. Expensive SSTable construction happened
   * before taking manifest_mutex_.
   */
  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    manifest_.applyEdit(edit);
  }

  /*
   * Only after the MANIFEST commit succeeds may the immutable slot be cleared.
   * A foreground writer blocked by backpressure can now rotate its active
   * Memtable safely.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (immutable_memtable_ != memtable) {
      throw std::logic_error("Flushed Memtable is not the current immutable");
    }

    immutable_memtable_.reset();
  }

  immutable_cleared_cv_.notify_all();
}

void KronosEngine::maybeCompact() {
  /*
   * Flush priority: if a writer has already installed another immutable
   * Memtable, return to the queue instead of beginning optional compaction.
   * A flush that arrives after compaction actually starts cannot be preempted,
   * but work already waiting never sits behind a newly-started compaction.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (immutable_memtable_) {
      return;
    }
  }

  std::vector<SstableMetadata> live_files;
  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    live_files = manifest_.liveFiles();
  }

  auto plan = compaction_policy_.pickCompaction(live_files);

  if (!plan.has_value()) {
    return;
  }

  std::vector<std::filesystem::path> input_paths;
  input_paths.reserve(plan->input_files.size());

  for (const auto &metadata : plan->input_files) {
    input_paths.push_back(metadata.path);
  }

  const auto output_path = allocateSstablePath(plan->output_level);

  const auto result =
      compactor_.compact(input_paths, output_path, sstable_block_size_,
                         bloom_bits_per_key_, plan->can_drop_tombstones);

  ManifestEdit edit;
  edit.remove_files = result.input_files;

  for (const auto &path : result.output_files) {
    edit.add_files.push_back(metadataForSstable(path, plan->output_level));
  }

  // Compactor output is not authoritative until this commit succeeds.
  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    manifest_.applyEdit(edit);
  }

  /*
   * Obsolete compaction inputs intentionally remain on disk. Module 9 will add
   * crash-recovery/orphan-obsolete cleanup after MANIFEST establishes logical
   * truth. Keeping them here also makes foreground readers holding an older
   * MANIFEST snapshot safe during concurrent compaction.
   */
}

void KronosEngine::backgroundWorkerLoop() {
  while (true) {
    auto task = flush_queue_.wait_and_pop();

    // Queue shutdown + fully drained accepted work => worker exits naturally.
    if (!task.has_value()) {
      break;
    }

    try {
      flushMemtable(*task);
      maybeCompact();

    } catch (...) {
      /*
       * Never allow an exception to escape a std::thread entry function:
       * uncaught exceptions there call std::terminate(). Record the failure,
       * wake any backpressured writers, close the queue, and let foreground
       * operations surface the original exception.
       */
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (!background_error_) {
          background_error_ = std::current_exception();
        }
      }

      immutable_cleared_cv_.notify_all();
      flush_queue_.shutdown();
      break;
    }
  }
}

void KronosEngine::shutdown() {
  /*
   * Phase 1: publish engine shutdown and wake backpressured writers. New writes
   * that enter after this point fail before touching the WAL.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (shutting_down_) {
      return;
    }

    shutting_down_ = true;
  }

  immutable_cleared_cv_.notify_all();

  /*
   * Phase 2: wait for any foreground write already in progress. A writer that
   * was sleeping on immutable_cleared_cv_ wakes because shutting_down_ is now
   * true, aborts before WAL append, and releases write_mutex_.
   */
  std::unique_lock<std::mutex> writer_lock(write_mutex_);

  /*
   * Before closing the queue, gracefully flush the final active Memtable. If an
   * older immutable is still being flushed, wait for that single slot first.
   */
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);

    immutable_cleared_cv_.wait(state_lock, [this] {
      return !immutable_memtable_ || background_error_;
    });

    if (!background_error_ && active_memtable_ &&
        active_memtable_->entry_count() > 0) {

      if (!active_memtable_->freeze()) {
        throw std::logic_error("Final active Memtable could not be frozen");
      }

      auto final_memtable =
          std::shared_ptr<Memtable>(std::move(active_memtable_));
      immutable_memtable_ = final_memtable;

      if (!flush_queue_.push(std::move(final_memtable))) {
        background_error_ = std::make_exception_ptr(std::runtime_error(
            "Flush queue closed before final Memtable could be scheduled"));
      }
    }
  }

  writer_lock.unlock();

  /*
   * Phase 3: close the queue to new work. Its graceful-drain contract guarantees
   * the final Memtable accepted above is processed before wait_and_pop() returns
   * nullopt to the worker.
   */
  flush_queue_.shutdown();

  /*
   * join() does not stop the worker; it waits until backgroundWorkerLoop()
   * actually returns. Never hold state_mutex_ or manifest_mutex_ while joining,
   * because the worker may need those mutexes in order to finish.
   */
  if (background_worker_.joinable()) {
    background_worker_.join();
  }

  // Explicit shutdown surfaces a background maintenance failure after cleanup.
  std::exception_ptr error;
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    error = background_error_;
  }

  if (error) {
    std::rethrow_exception(error);
  }
}

} // namespace kronos
