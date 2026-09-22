#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <kronos/compaction_policy.hpp>
#include <kronos/compactor.hpp>
#include <kronos/config.hpp>
#include <kronos/manifest.hpp>
#include <kronos/memtable.hpp>
#include <kronos/sstable.hpp>
#include <kronos/thread_safe_queue.hpp>
#include <kronos/types.hpp>
#include <kronos/wal.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
namespace kronos {

struct EngineMetrics {
  size_t flush_count = 0;
  size_t sstable_count = 0;
  size_t compaction_count = 0;
  std::chrono::nanoseconds total_compaction_time{0};
};

/*
 * KronosEngine
 * ------------
 * Owns and coordinates the storage-engine components that earlier modules
 * implemented independently.
 *
 * Module 8 concurrency model (v1):
 *
 *   - one foreground write path at a time,
 *   - one active mutable Memtable,
 *   - at most one immutable Memtable,
 *   - one serialized background maintenance worker,
 *   - foreground reads may continue while an immutable Memtable is flushing,
 *   - writers apply backpressure when the immutable slot is occupied,
 *   - MANIFEST remains the authoritative logical SSTable state.
 *
 * The background worker performs flushes and then invokes the already-existing
 * compaction policy/compactor when L0 requires maintenance.
 */
class KronosEngine {
public:
  explicit KronosEngine(const Config &config,
                        const std::filesystem::path &db_path);

  ~KronosEngine();

  KronosEngine(const KronosEngine &) = delete;
  KronosEngine &operator=(const KronosEngine &) = delete;
  KronosEngine(KronosEngine &&) = delete;
  KronosEngine &operator=(KronosEngine &&) = delete;

  void put(const std::string &key, const std::string &value);
  GetResult get(const std::string &key);
  void remove(const std::string &key);

  // Gracefully stop accepting new writes, flush accepted in-memory work,
  // drain the background queue, and join the worker thread.
  // Calling shutdown() more than once is safe.
  void shutdown();
  EngineMetrics getMetrics() const;

private:
  std::filesystem::path db_path_;

  // Config-derived limits retained by the engine because fresh Memtables are
  // created repeatedly during active -> immutable rotation.
  size_t memtable_target_bytes_;

  // Chronos v1 SSTable defaults. These were already used by the SSTable layer;
  // making them engine members keeps flush/compaction output consistent.
  size_t sstable_block_size_ = 4096;
  size_t bloom_bits_per_key_ = 10;

  // Only the serialized maintenance worker creates SSTables in Module 8, so a
  // simple monotonically increasing allocator is sufficient.
  // allocateSstablePath still checks the filesystem to avoid collisions after
  // restart/orphan files.
  uint64_t next_sstable_id_ = 1;

  Wal wal_;
  CompactionPolicy compaction_policy_;
  Compactor compactor_;

  // The foreground owns the active Memtable exclusively.
  std::unique_ptr<Memtable> active_memtable_;

  // The immutable Memtable has shared lifetime because foreground GETs may
  // still read it while the background worker is flushing it.
  std::shared_ptr<Memtable> immutable_memtable_;

  Manifest manifest_;

  // The queue carries frozen Memtables. Compaction itself is discovered by the
  // worker from MANIFEST state after flushes rather than being separately
  // queued.
  thread_safe_queue<std::shared_ptr<Memtable>> flush_queue_;

  // Serializes foreground writers. This keeps WAL sequence allocation ordered
  // without turning the WAL itself into a multi-writer component.
  std::mutex write_mutex_;

  // Protects active/immutable ownership, shutdown state and background_error_.
  std::mutex state_mutex_;

  // Manifest is read by foreground GETs and edited by the maintenance worker.
  // Never hold manifest_mutex_ while performing expensive compaction I/O.
  mutable std::mutex manifest_mutex_;

  // Writers sleep here when an active Memtable needs rotation but the single
  // immutable slot is still occupied.
  std::condition_variable immutable_cleared_cv_;

  // If maintenance fails, the worker records the exception and wakes blocked
  // foreground writers instead of terminating the process or blocking forever.
  std::exception_ptr background_error_;

  bool shutting_down_ = false;

  void recover();
  // Declared after every state object it may access. The thread is started only
  // in the constructor body after all members above are fully constructed.
  std::thread background_worker_;

  void backgroundWorkerLoop();

  // The caller must hold state_mutex_.
  void prepareForWriteLocked(std::unique_lock<std::mutex> &state_lock,
                             const std::string &key,
                             const InternalEntry &candidate);

  // The caller must hold state_mutex_; immutable_memtable_ must be empty.
  void rotateActiveMemtableLocked();

  // Flush one frozen Memtable to L0 and commit it to MANIFEST. The immutable
  // slot is cleared only after the MANIFEST commit succeeds.
  void flushMemtable(const std::shared_ptr<Memtable> &memtable);

  // Run the existing Module 7 compaction mechanism if the policy requests it.
  void maybeCompact();

  // Build exact key-range metadata for an already-finalized SSTable.
  SstableMetadata metadataForSstable(const std::filesystem::path &path,
                                     size_t level) const;

  std::filesystem::path allocateSstablePath(size_t level);

  // Must be called while state_mutex_ is held.
  void rethrowBackgroundErrorLocked() const;
  // to validate an sstable
  void validateManifestSstables() const;

  std::atomic<size_t> flush_count_{0};
  std::atomic<size_t> compaction_count_{0};
  std::atomic<int64_t> total_compaction_time_ns_{0};

  // Reader cache for easier reads
  mutable std::mutex sstable_reader_mutex_;
  std::unordered_map<std::string, std::shared_ptr<SstableReader>>
      sstable_readers_;
  std::shared_ptr<SstableReader>
  getSstableReader(const std::filesystem::path &path);
};

} // namespace kronos
