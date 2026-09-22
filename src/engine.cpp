

#include <algorithm>
#include <atomic>
#include <chrono>
#include <kronos/engine.hpp>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kronos {

namespace {

std::filesystem::path
prepareDatabaseDirectory(const std::filesystem::path &db_path) {
  if (db_path.empty()) {
    throw std::invalid_argument("Database path cannot be empty");
  }

  std::filesystem::create_directories(db_path);
  return db_path;
}

/* To converts the user's megabyte configuration value into bytes, performing
  overflow and boundary checks.*/
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
   * Reconstruct persistent database state before any background activity
   * begins. If recovery fails, construction fails and no worker is started.
   */
  recover();

  /*
   * Recovery has completed successfully. It is now safe for background
   * maintenance to begin.
   */
  background_worker_ = std::thread(&KronosEngine::backgroundWorkerLoop, this);
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
    // Catch ALL exceptions (...) and swallow them quietly.
  }
}

void KronosEngine::rethrowBackgroundErrorLocked() const {
  if (background_error_) {
    std::rethrow_exception(background_error_);
  }
}

// To manage memory capacity and apply backpressure to ensure that the engine
// never runs out of memory while waiting for the disk to catch up.
void KronosEngine::prepareForWriteLocked(
    std::unique_lock<std::mutex> &state_lock, const std::string &key,
    const InternalEntry &candidate) {

  // It ensures the caller passed an active lock on state_mutex_
  if (!state_lock.owns_lock()) {
    throw std::logic_error("prepareForWriteLocked requires state_mutex_");
  }

  // If the database is shutting down, reject the write immediately.
  if (shutting_down_) {
    throw std::runtime_error("KronosEngine is shutting down");
  }

  rethrowBackgroundErrorLocked();

  /*
    A single record may legitimately be larger than the configured Memtable
    target. Rotating an empty Memtable would create an empty immutable table,
    which cannot be flushed to a valid SSTable. Therefore size-triggered
    rotation is meaningful only when the active Memtable already contains at
    least one record.
   */
  auto rotation_required = [&] {
    return active_memtable_->entry_count() > 0 &&
           active_memtable_->would_exceed_target(key, candidate);
  };

  /*
    If the active Memtable needs to rotate while the one immutable slot is
    occupied, the foreground writer sleeps. condition_variable::wait releases
    state_mutex_ while sleeping, allowing the background worker to commit the
    immutable Memtable, clear the slot, and notify us.
   */
  while (rotation_required() && immutable_memtable_) {
    // we put the foreground writer thread to sleep
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

  // to check Slot Capacity
  if (immutable_memtable_) {
    throw std::logic_error("Immutable Memtable slot is already occupied");
  }

  // Empty Table
  if (active_memtable_->entry_count() == 0) {
    return;
  }

  if (!active_memtable_->freeze()) {
    throw std::logic_error("Active Memtable could not be frozen");
  }

  // Transfer ownership from the foreground's unique_ptr into shared lifetime.
  /*
    Foreground GETs and the background flush worker may now read the same
    frozen Memtable concurrently, but nobody may mutate it.
*/

  // Relinquishes ownership from the foreground writer's exclusive
  // std::unique_ptr, and wraps the frozen Memtable in a std::shared_ptr.
  auto frozen = std::shared_ptr<Memtable>(std::move(active_memtable_));
  //   Assigns the shared pointer to the immutable_memtable_ slot.
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
   * requiring the WAL itself to become a multi-writer component.

   * Reads do not take write_mutex_ and may proceed concurrently.
   */

  // write_mutex_ ensures only one writer reaches wal_.put(...) at a time.
  std::lock_guard<std::mutex> writer_lock(write_mutex_);

  // Sequence does not affect the size estimate. UINT64_MAX makes the candidate
  // unambiguously newer if this key already exists in the active Memtable.
  InternalEntry candidate{value, std::numeric_limits<uint64_t>::max(),
                          OperationType::PUT};

  {

    std::unique_lock<std::mutex> state_lock(state_mutex_);
    prepareForWriteLocked(state_lock, key, candidate);
  }

  // Before touching RAM, the key-value pair is appended to the WAL on disk.
  const uint64_t sequence = wal_.put(key, value);

  {
    // to safely mutate active_memtable_
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

  /**
   * We temporarily keep a shared reference to the current immutable Memtable.
   *
   * Once a Memtable becomes immutable, nobody modifies it anymore.
   * Therefore, after taking this shared_ptr snapshot, we can safely read the
   * Memtable without continuing to hold state_mutex_.
   */
  std::shared_ptr<Memtable> immutable_snapshot;

  /**
   * STEP 1: Check the active Memtable.
   *
   * The active Memtable is mutable, so foreground writers may currently be
   * changing its std::map. We therefore have to inspect it while holding
   * state_mutex_.
   *
   * Chronos v1 has an important ordering invariant:
   *
   *     active Memtable
   *         ↓ newer than
   *     immutable Memtable
   *         ↓ newer than
   *     flushed SSTables
   *
   * Sequence numbers increase with every WAL-backed write.
   *
   * Therefore, if the key exists in the active Memtable, that entry is already
   * the newest visible version of the key. There is no reason to inspect the
   * immutable Memtable or any SSTables.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (active_memtable_) {

      auto active_entry = active_memtable_->lookupEntry(key);

      if (active_entry.has_value()) {

        // A tombstone is also a valid result. We must return DELETED here
        // rather than continuing to disk and accidentally resurrecting an
        // older value.
        if (active_entry->operation == OperationType::DELETE) {
          return {.value = "", .status = GetStatus::DELETED};
        }

        return {.value = active_entry->value, .status = GetStatus::FOUND};
      }
    }

    /**
     * The key was not present in the active Memtable.
     *
     * Take a shared_ptr snapshot of the current immutable Memtable while the
     * engine state is still protected.
     *
     * This does NOT copy the Memtable itself. Both shared_ptrs temporarily
     * refer to the same frozen Memtable.
     */
    immutable_snapshot = immutable_memtable_;
  }

  /**
   * STEP 2: Check the immutable Memtable.
   *
   * We no longer need state_mutex_ here because:
   *
   *   1. the Memtable is frozen and cannot be modified;
   *   2. immutable_snapshot keeps it alive even if the background worker
   *      finishes flushing it and clears immutable_memtable_.
   *
   * If the key exists here, it is newer than every version already persisted
   * in the SSTables, so we can return immediately.
   */
  if (immutable_snapshot) {

    auto immutable_entry = immutable_snapshot->lookupEntry(key);

    if (immutable_entry.has_value()) {

      if (immutable_entry->operation == OperationType::DELETE) {
        return {.value = "", .status = GetStatus::DELETED};
      }

      return {.value = immutable_entry->value, .status = GetStatus::FOUND};
    }
  }

  /**
   * STEP 3: The key is absent from both in-memory generations.
   *
   * We now have to search the SSTables.
   *
   * Unlike active vs immutable vs disk, SSTables cannot simply be trusted
   * based on the order in which we encounter them. Multiple live SSTables may
   * contain different historical versions of the same key.
   *
   * Therefore, among SSTable candidates, the highest sequence number still
   * wins.
   */
  std::optional<InternalEntry> winner;

  /**
   * Copy the current MANIFEST state while holding manifest_mutex_, then release
   * the mutex before doing file I/O.
   *
   * The background worker may commit a newer MANIFEST after this snapshot.
   * Module 8 deliberately does not physically delete obsolete SSTables, so
   * files referenced by this snapshot remain safe for this GET to read.
   */
  std::vector<SstableMetadata> live_files;

  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);

    live_files = manifest_.liveFiles();
  }

  for (const auto &metadata : live_files) {

    /**
     * Cheap range exclusion.
     *
     * Example:
     *
     * SSTable range = apple .. mango
     * requested key = zebra
     *
     * zebra cannot possibly exist inside this SSTable, so don't even open it.
     */
    if (key < metadata.smallest_key || key > metadata.largest_key) {
      continue;
    }

    SstableReader reader(metadata.path);

    /**
     * Several SSTables may contain different versions of this key.
     *
     * Here we still need the same rule used by the Compactor:
     *
     *     highest sequence number wins.
     */
    considerNewer(winner, reader.lookupEntry(key));
  }

  /**
   * No version existed in:
   *
   *   active Memtable
   *   immutable Memtable
   *   any live SSTable
   */
  if (!winner.has_value()) {
    return {.value = "", .status = GetStatus::NOT_FOUND};
  }

  /**
   * The newest persisted version is a tombstone.
   *
   * An older PUT may still physically exist in another SSTable, but it must
   * remain logically deleted.
   */
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

  /*
   * The recovery checkpoint for this flush is the highest sequence number
   * represented by this immutable Memtable generation.
   *
   *
   * Because foreground writes receive monotonically increasing sequence
   * numbers and Memtable generations do not overlap, once this entire
   * Memtable becomes authoritative in MANIFEST, the logical effects of all
   * writes through this sequence are covered by SSTable state.
   */
  uint64_t flushed_through = iterator.entry().sequence;

  while (iterator.valid()) {

    largest_key = iterator.key();

    flushed_through = std::max(flushed_through, iterator.entry().sequence);

    builder.add(iterator.key(), iterator.entry());

    iterator.next();
  }

  /*
   * Complete the SSTable before MANIFEST is allowed to reference it.
   *
   * If SSTable creation fails, MANIFEST remains unchanged and the Memtable
   * is still the authoritative in-memory copy.
   */
  builder.finish();

  /*
   * The SSTable addition and recovery checkpoint advance belong to the same
   * MANIFEST transition.
   *
   * This prevents a crash from exposing a checkpoint that refers to data
   * which has not yet become authoritative.
   */
  ManifestEdit edit;

  edit.add_files.push_back(
      SstableMetadata{output_path, 0, smallest_key, largest_key});

  edit.persisted_through_ = flushed_through;

  /*
   * Commit logical truth.
   *
   * After this succeeds:
   *
   *   - the new SSTable is authoritative;
   *   - the MANIFEST checkpoint says that writes through flushed_through
   *     no longer need to be reconstructed from the WAL.
   */
  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    manifest_.applyEdit(edit);
  }
  flush_count_.fetch_add(1, std::memory_order_relaxed);

  /*
   * Only after the MANIFEST commit succeeds may this immutable Memtable
   * stop being part of the live database state.
   *
   * Writers blocked by the single immutable-Memtable slot may proceed once
   * the slot is cleared.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (immutable_memtable_ != memtable) {
      throw std::logic_error("Flushed Memtable is not the current immutable");
    }

    /*
     * The MANIFEST has already committed this Memtable's SSTable and
     * checkpoint, so the immutable slot can now be released.
     */
    immutable_memtable_.reset();
  }

  /*
   * Wake writers and shutdown() before attempting WAL reclamation.
   *
   * This ordering is critical:
   *
   * shutdown() may hold write_mutex_ while waiting for the immutable slot.
   * If the worker tried to acquire write_mutex_ before clearing and notifying
   * the immutable slot, the two threads would deadlock.
   */
  immutable_cleared_cv_.notify_all();

  /*
   * WAL reclamation must be serialized against foreground WAL appends.
   *
   * At this point the immutable slot is already clear, so a thread holding
   * write_mutex_ is no longer waiting for this worker to make progress.
   */
  {
    std::lock_guard<std::mutex> writer_lock(write_mutex_);
    wal_.reclaimThrough(flushed_through);
  }
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
  const auto compaction_start = std::chrono::steady_clock::now();
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

  const auto compaction_end = std::chrono::steady_clock::now();

  const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      compaction_end - compaction_start);

  compaction_count_.fetch_add(1, std::memory_order_relaxed);

  total_compaction_time_ns_.fetch_add(duration.count(),
                                      std::memory_order_relaxed);

  /*
   * Obsolete compaction inputs intentionally remain on disk. Module 9 will add
   * crash-recovery/orphan-obsolete cleanup after MANIFEST establishes logical
   * truth. Keeping them here also makes foreground readers holding an older
   * MANIFEST snapshot safe during concurrent compaction.
   */
}

void KronosEngine::validateManifestSstables() const {

  const auto &live_files = manifest_.liveFiles();

  for (const auto &metadata : live_files) {

    if (!std::filesystem::exists(metadata.path)) {
      throw std::runtime_error("MANIFEST references a missing SSTable: " +
                               metadata.path.string());
    }

    /*
     * Constructing SstableReader validates the SSTable's structural
     * metadata: header/version, footer, Bloom-filter metadata and
     * sparse-index metadata.
     *
     * Data-block CRCs remain validated when those blocks are actually read.
     */
    SstableReader reader(metadata.path);
  }
}

void KronosEngine::recover() {

  /*
   * MANIFEST is authoritative. Before modifying/replaying the WAL,
   * verify that every SSTable it declares live can actually be opened.
   */
  validateManifestSstables();
  /*
   * Read and validate every record that is still present in the WAL.
   *
   * wal_.recover() also repairs an incomplete final record and restores
   * its own next-sequence position from the WAL records it discovers.
   */
  const auto records = wal_.recover();

  /*
   * The MANIFEST checkpoint tells us how far persistent SSTable state
   * already covers the logical write history.
   *
   * Any WAL record at or below this boundary is historical redundancy
   * and must not be replayed into the active Memtable.
   */
  const auto checkpoint = manifest_.persistedThrough();

  std::optional<uint64_t> previous_sequence;

  for (const auto &record : records) {

    /*
     * WAL records produced by Chronos must have strictly increasing
     * sequence numbers.
     *
     * A regression or duplicate sequence indicates an invalid WAL history.
     */
    if (previous_sequence.has_value() &&
        record.sequence <= *previous_sequence) {
      throw std::runtime_error(
          "WAL recovery found non-increasing sequence numbers");
    }

    previous_sequence = record.sequence;

    /*
     * There is no usable sequence number after UINT64_MAX.
     * Refuse to open the database rather than allowing the allocator to wrap.
     */
    if (record.sequence == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("WAL sequence space exhausted");
    }

    /*
     * Records through the MANIFEST checkpoint are already represented by
     * authoritative SSTable state.
     *
     * Only records newer than the checkpoint belonged to in-memory state
     * that was lost when the previous process stopped.
     */
    if (checkpoint.has_value() && record.sequence <= *checkpoint) {
      continue;
    }

    /*
     * Replay the original operation directly into the active Memtable.
     *
     * We deliberately do NOT call KronosEngine::put()/remove() here because
     * those functions would append a second WAL record and allocate a new
     * sequence number.
     *
     * Recovery must restore the original sequence exactly.
     */
    if (record.operation == OperationType::PUT) {

      const auto result =
          active_memtable_->put(record.key, record.value, record.sequence);

      if (result != Memtable::WriteResult::SUCCESS) {
        throw std::logic_error("Active Memtable rejected recovered WAL PUT");
      }

    } else if (record.operation == OperationType::DELETE) {

      const auto result = active_memtable_->remove(record.key, record.sequence);

      if (result != Memtable::WriteResult::SUCCESS) {
        throw std::logic_error("Active Memtable rejected recovered WAL DELETE");
      }

    } else {

      /*
       * Wal::recover() should already reject invalid operation bytes.
       * Keep this defensive check so the engine never silently ignores an
       * unknown recovered operation.
       */
      throw std::runtime_error("WAL recovery produced an invalid operation");
    }
  }

  /*
   * The WAL may contain no records newer than the checkpoint.
   *
   * Example:
   *
   *   MANIFEST checkpoint = 134
   *   WAL                  = empty
   *
   * The next write must still receive sequence 135 rather than restarting
   * from zero.
   *
   * ensureNextSequenceAtLeast() cannot move an already-higher WAL sequence
   * backwards.
   */
  if (checkpoint.has_value()) {

    if (*checkpoint == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("WAL sequence space exhausted");
    }

    wal_.ensureNextSequenceAtLeast(*checkpoint + 1);
  }
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
   * Phase 1:
   * Publish shutdown exactly once.
   */
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (shutting_down_) {
      return;
    }

    shutting_down_ = true;
  }

  /*
   * Wake any foreground writer blocked on the immutable slot.
   */
  immutable_cleared_cv_.notify_all();

  /*
   * Any exception that happens during graceful shutdown must NOT escape
   * immediately.
   *
   * We first remember it, then always shut down the queue and join the worker.
   *
   * Otherwise:
   *
   *   shutdown throws
   *       ↓
   *   destructor calls shutdown again
   *       ↓
   *   shutting_down_ is already true
   *       ↓
   *   shutdown returns
   *       ↓
   *   std::thread is still joinable
   *       ↓
   *   std::terminate()
   */
  std::exception_ptr shutdown_error = nullptr;

  try {
    /*
     * Phase 2:
     * Foreground-writer barrier.
     *
     * If a writer entered before shutting_down_ became true, wait until it
     * releases write_mutex_.
     *
     * IMPORTANT:
     * Do not keep write_mutex_ while waiting for background flushing because
     * flushMemtable() also needs it for WAL reclamation.
     */
    { std::unique_lock<std::mutex> writer_barrier(write_mutex_); }

    /*
     * Phase 3:
     * Wait for any existing immutable Memtable to finish.
     */
    {
      std::unique_lock<std::mutex> state_lock(state_mutex_);

      immutable_cleared_cv_.wait(state_lock, [this] {
        return !immutable_memtable_ || background_error_;
      });

      /*
       * If background maintenance already failed, do not schedule any more
       * work. We will surface that failure after joining the worker.
       */
      if (!background_error_ && active_memtable_ &&
          active_memtable_->entry_count() > 0) {

        /*
         * Persist the final active Memtable before graceful shutdown
         * completes.
         */
        if (!active_memtable_->freeze()) {
          throw std::logic_error("Final active Memtable could not be frozen");
        }

        auto final_memtable =
            std::shared_ptr<Memtable>(std::move(active_memtable_));

        immutable_memtable_ = final_memtable;

        /*
         * The queue must still accept this final flush.
         */
        if (!flush_queue_.push(std::move(final_memtable))) {
          throw std::runtime_error("Flush queue closed before final Memtable "
                                   "could be scheduled");
        }
      }
    }

  } catch (...) {
    /*
     * Do NOT rethrow yet.
     *
     * We still own a potentially joinable background thread.
     */
    shutdown_error = std::current_exception();
  }

  /*
   * Phase 4:
   * Always close the queue.
   *
   * Already accepted work is allowed to drain.
   */
  flush_queue_.shutdown();

  /*
   * Phase 5:
   * Always join the worker before allowing shutdown() to return or throw.
   */
  try {
    if (background_worker_.joinable()) {
      background_worker_.join();
    }

  } catch (...) {
    /*
     * Preserve an earlier shutdown failure if one already exists.
     */
    if (!shutdown_error) {
      shutdown_error = std::current_exception();
    }
  }

  /*
   * Capture any failure reported by the background worker.
   */
  std::exception_ptr background_error;

  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    background_error = background_error_;
  }

  /*
   * Only NOW is it safe for shutdown() to throw because the worker is no
   * longer joinable.
   */
  if (shutdown_error) {
    std::rethrow_exception(shutdown_error);
  }

  if (background_error) {
    std::rethrow_exception(background_error);
  }
}

EngineMetrics KronosEngine::getMetrics() const {
  EngineMetrics metrics;

  metrics.flush_count = flush_count_.load(std::memory_order_relaxed);

  metrics.compaction_count = compaction_count_.load(std::memory_order_relaxed);

  metrics.total_compaction_time = std::chrono::nanoseconds(
      total_compaction_time_ns_.load(std::memory_order_relaxed));

  {
    std::lock_guard<std::mutex> manifest_lock(manifest_mutex_);
    metrics.sstable_count = manifest_.liveFiles().size();
  }

  return metrics;
}

} // namespace kronos
