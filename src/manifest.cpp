#include "kronos/manifest.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace kronos {

namespace {

/*
 * Identifies the current on-disk MANIFEST format.
 *
 * If the format changes in the future, the version can change
 * without silently interpreting an incompatible MANIFEST.
 */
constexpr const char *MANIFEST_MAGIC = "KRONOS_MANIFEST_V2";

void fsyncFilePath(const std::filesystem::path &path) {

  int fd = ::open(path.c_str(), O_RDONLY);

  if (fd == -1) {
    throw std::runtime_error("Failed to open file for fsync: " + path.string());
  }

  while (::fsync(fd) == -1) {

    if (errno == EINTR) {
      continue;
    }

    ::close(fd);

    throw std::runtime_error("Failed to fsync file: " + path.string());
  }

  ::close(fd);
}

/*
 * Persist the directory entry containing a rename/create operation.
 *
 * On filesystems/platforms that do not support fsync() on directories,
 * EINVAL/ENOTSUP is treated as "unsupported" rather than as database
 * corruption.
 *
 * Therefore our automated macOS tests prove process-crash recovery.
 * The directory-fsync protocol is primarily the POSIX/Linux power-loss
 * durability path.
 */
void fsyncParentDirectory(const std::filesystem::path &path) {

  std::filesystem::path parent = path.parent_path();

  if (parent.empty()) {
    parent = ".";
  }

  int fd = ::open(parent.c_str(), O_RDONLY);

  if (fd == -1) {
    throw std::runtime_error("Failed to open parent directory for fsync: " +
                             parent.string());
  }

  while (::fsync(fd) == -1) {

    if (errno == EINTR) {
      continue;
    }

    if (errno == EINVAL || errno == ENOTSUP) {
      ::close(fd);
      return;
    }

    ::close(fd);

    throw std::runtime_error("Failed to fsync parent directory: " +
                             parent.string());
  }

  ::close(fd);
}
/*
 * Keep MANIFEST state deterministic.
 *
 * Ordering is not required for logical correctness, but deterministic
 * ordering makes debugging, testing and inspecting the file easier.
 *
 * Files are ordered:
 *
 *   1. by level
 *   2. by path within that level
 */

void sortMetadata(std::vector<SstableMetadata> &files) {

  std::sort(files.begin(), files.end(),
            [](const SstableMetadata &a, const SstableMetadata &b) {
              if (a.level != b.level) {
                return a.level < b.level;
              }

              return a.path.string() < b.path.string();
            });
}

/*
 * Validate metadata before allowing it into authoritative state.
 */
void validateMetadata(const SstableMetadata &metadata) {

  if (metadata.path.empty()) {
    throw std::invalid_argument("SSTable metadata path cannot be empty");
  }

  if (metadata.smallest_key > metadata.largest_key) {
    throw std::invalid_argument("Invalid SSTable key range");
  }
}

} // namespace

/*
 * Open the MANIFEST and reconstruct the authoritative state.
 *
 * A leftover .tmp file represents a snapshot that was being prepared
 * but never reached the logical commit point.
 *
 * Therefore it must not become authoritative.
 */
Manifest::Manifest(std::filesystem::path manifest_path)
    : manifest_path_(std::move(manifest_path)) {

  std::error_code ec;

  std::filesystem::remove(temporaryPath(), ec);

  load();
}

/*
 * Build the temporary MANIFEST path.
 *
 * Example:
 *
 *   db/MANIFEST
 *       ↓
 *   db/MANIFEST.tmp
 */
std::filesystem::path Manifest::temporaryPath() const {

  return std::filesystem::path(manifest_path_.string() + ".tmp");
}

/*
 * Return the complete authoritative in-memory live-file set.
 */
const std::vector<SstableMetadata> &Manifest::liveFiles() const noexcept {

  return live_files_;
}

std::optional<uint64_t> Manifest::persistedThrough() const noexcept {
  return persisted_through_;
}

/*
 * Return only the live SSTables belonging to a specific level.
 */
std::vector<SstableMetadata> Manifest::filesAtLevel(size_t level) const {

  std::vector<SstableMetadata> result;

  for (const auto &file : live_files_) {

    if (file.level == level) {
      result.push_back(file);
    }
  }

  return result;
}

/*
 * Check logical MANIFEST membership.
 *
 * This deliberately does NOT call filesystem::exists().
 *
 * A file existing physically on disk does not necessarily mean
 * that Chronos considers it live.
 */
bool Manifest::contains(const std::filesystem::path &path) const {

  return std::any_of(
      live_files_.begin(), live_files_.end(),
      [&](const SstableMetadata &metadata) { return metadata.path == path; });
}

/*
 * Reload the authoritative MANIFEST from disk.
 *
 * Important:
 *
 * We parse into loaded_files first rather than modifying live_files_
 * incrementally.
 *
 * This means a malformed MANIFEST cannot leave the in-memory state
 * half-reconstructed.
 *
 * Only after the complete file has successfully parsed and validated
 * do we replace live_files_.
 */
void Manifest::load() {

  /*
   * No MANIFEST means this database has no previously committed
   * persistent SSTable state or recovery checkpoint.
   */
  if (!std::filesystem::exists(manifest_path_)) {
    live_files_.clear();
    persisted_through_ = std::nullopt;
    return;
  }

  std::ifstream input(manifest_path_, std::ios::binary);

  if (!input) {
    throw std::runtime_error("Failed to open MANIFEST");
  }

  /*
   * Reconstruct the MANIFEST into temporary state first.
   *
   * We do not modify the current in-memory authoritative state until
   * the entire file has been parsed and validated successfully.
   */
  std::vector<SstableMetadata> loaded_files;
  std::optional<uint64_t> loaded_checkpoint = std::nullopt;

  // -----------------------------------------------------------------------
  // 1. Validate MANIFEST format version
  // -----------------------------------------------------------------------

  std::string magic;
  std::getline(input, magic);

  if (magic != MANIFEST_MAGIC) {
    throw std::runtime_error("Invalid MANIFEST header");
  }

  // -----------------------------------------------------------------------
  // 2. Recover the persisted WAL checkpoint
  // -----------------------------------------------------------------------

  std::string checkpoint_label;
  std::string checkpoint_value;

  if (!(input >> checkpoint_label >> checkpoint_value)) {
    throw std::runtime_error("Missing MANIFEST checkpoint");
  }

  if (checkpoint_label != "CHECKPOINT") {
    throw std::runtime_error("Invalid MANIFEST checkpoint header");
  }

  /*
   * CHECKPOINT NONE means no Memtable generation has yet been committed
   * to authoritative SSTable state.
   *
   * Otherwise the value is the highest sequence whose logical effects
   * are known to be represented by authoritative SSTables.
   */
  if (checkpoint_value != "NONE") {
    uint64_t parsed = 0;

    const char *begin = checkpoint_value.data();
    const char *end = begin + checkpoint_value.size();

    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);

    if (ec == std::errc::result_out_of_range) {
      throw std::runtime_error("MANIFEST checkpoint is out of range");
    }

    /*
     * Reject:
     *
     *   -1
     *   +1
     *   abc
     *   100abc
     *   empty/otherwise malformed numeric tokens
     */
    if (ec != std::errc{} || ptr != end) {
      throw std::runtime_error("Invalid MANIFEST checkpoint value");
    }

    loaded_checkpoint = parsed;
  }

  // -----------------------------------------------------------------------
  // 3. Reconstruct the authoritative SSTable set
  // -----------------------------------------------------------------------

  while (true) {

    size_t level;

    /*
     * Clean EOF here means every MANIFEST record has been consumed.
     */
    if (!(input >> level)) {
      break;
    }

    std::string path_string;
    std::string smallest_key;
    std::string largest_key;

    /*
     * SSTable record format:
     *
     * [level] [quoted path] [quoted smallest key] [quoted largest key]
     *
     * std::quoted() preserves spaces inside paths and keys.
     */
    if (!(input >> std::quoted(path_string) >> std::quoted(smallest_key) >>
          std::quoted(largest_key))) {

      throw std::runtime_error("Malformed MANIFEST record");
    }

    SstableMetadata metadata{std::filesystem::path(path_string), level,
                             smallest_key, largest_key};

    validateMetadata(metadata);

    /*
     * One physical SSTable path may appear only once in the
     * authoritative live-file set.
     */
    const bool duplicate = std::any_of(loaded_files.begin(), loaded_files.end(),
                                       [&](const SstableMetadata &existing) {
                                         return existing.path == metadata.path;
                                       });

    if (duplicate) {
      throw std::runtime_error("Duplicate SSTable in MANIFEST");
    }

    loaded_files.push_back(std::move(metadata));
  }

  /*
   * Extraction failure is acceptable only if we actually reached EOF.
   * Otherwise some malformed token stopped MANIFEST parsing.
   */
  if (!input.eof()) {
    throw std::runtime_error("Malformed MANIFEST");
  }

  sortMetadata(loaded_files);

  // -----------------------------------------------------------------------
  // 4. Publish the fully validated MANIFEST state
  // -----------------------------------------------------------------------

  /*
   * Only now do the recovered values become the Manifest object's
   * authoritative in-memory view.
   *
   * If any earlier parsing or validation step had failed, these members
   * would have remained unchanged.
   */
  live_files_ = std::move(loaded_files);
  persisted_through_ = loaded_checkpoint;
}

/*
 * Persist one complete replacement snapshot.
 *
 * This function does NOT write only the ManifestEdit.
 *
 * Example:
 *
 * Current:
 *
 *   1.sst
 *   2.sst
 *   3.sst
 *
 * Edit:
 *
 *   remove 1.sst
 *   add    4.sst
 *
 * MANIFEST.tmp contains:
 *
 *   2.sst
 *   3.sst
 *   4.sst
 *
 * i.e. the entire new authoritative state.
 */
void Manifest::persistSnapshot(
    const std::vector<SstableMetadata> &files,
    const std::optional<uint64_t> &checkpoint) const {

  const auto parent = manifest_path_.parent_path();

  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  const auto temp_path = temporaryPath();

  /*
   * Build the complete next MANIFEST separately.
   *
   * Until rename succeeds, the existing MANIFEST remains authoritative.
   */
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);

    if (!output) {
      throw std::runtime_error("Failed to create temporary MANIFEST");
    }

    output << MANIFEST_MAGIC << '\n';

    output << "CHECKPOINT ";

    if (checkpoint.has_value()) {
      output << *checkpoint;
    } else {
      output << "NONE";
    }

    output << '\n';

    for (const auto &metadata : files) {

      validateMetadata(metadata);

      output << metadata.level << ' ' << std::quoted(metadata.path.string())
             << ' ' << std::quoted(metadata.smallest_key) << ' '
             << std::quoted(metadata.largest_key) << '\n';
    }

    /*
     * Push the C++ userspace buffer into the kernel before closing.
     */
    output.flush();

    if (!output) {
      throw std::runtime_error("Failed to write MANIFEST");
    }

    /*
     * Explicitly close so close-time stream failures are observed here
     * rather than being silently swallowed by the destructor.
     */
    output.close();

    if (!output) {
      throw std::runtime_error("Failed to close temporary MANIFEST");
    }
  }

  /*
   * flush() only moves bytes out of the C++ stream buffer.
   *
   * fsync() establishes the file-level durability boundary for the
   * fully-written temporary snapshot before it is allowed to become
   * authoritative.
   */
  fsyncFilePath(temp_path);

  /*
   * Logical commit boundary:
   *
   * BEFORE rename:
   *     old MANIFEST is authoritative
   *
   * AFTER rename:
   *     new MANIFEST is authoritative
   */
  std::error_code ec;

  std::filesystem::rename(temp_path, manifest_path_, ec);

  if (ec) {

    std::filesystem::remove(temp_path);

    throw std::runtime_error("Failed to commit MANIFEST");
  }

  /*
   * fsync(temp) persisted the file contents.
   *
   * fsync(parent directory) persists the directory-entry transition caused
   * by rename so the new MANIFEST name itself survives the supported
   * power-loss durability protocol.
   */
  fsyncParentDirectory(manifest_path_);
}

/*
 * Apply one logical transition to the authoritative live-file set.
 *
 * Important ordering:
 *
 *   copy current state
 *        ↓
 *   modify copy
 *        ↓
 *   persist complete replacement snapshot
 *        ↓
 *   commit via rename
 *        ↓
 *   update in-memory state
 *
 * At no point do we mutate live_files_ before the persistent commit
 * has succeeded.
 */
void Manifest::applyEdit(const ManifestEdit &edit) {

  /*
   * Build the next candidate MANIFEST state in memory first.
   *
   * Nothing becomes authoritative until persistSnapshot() succeeds.
   */
  auto next_state = live_files_;
  auto next_checkpoint = persisted_through_;

  // -----------------------------------------------------------------------
  // 1. Remove SSTables that should no longer be authoritative
  // -----------------------------------------------------------------------

  for (const auto &remove_path : edit.remove_files) {

    const auto old_size = next_state.size();

    next_state.erase(std::remove_if(next_state.begin(), next_state.end(),
                                    [&](const SstableMetadata &metadata) {
                                      return metadata.path == remove_path;
                                    }),
                     next_state.end());

    /*
     * Removing a file that is not currently live indicates an invalid
     * MANIFEST transition.
     */
    if (next_state.size() == old_size) {
      throw std::runtime_error(
          "MANIFEST edit tried to remove an unknown SSTable");
    }
  }

  // -----------------------------------------------------------------------
  // 2. Add newly authoritative SSTables
  // -----------------------------------------------------------------------

  for (const auto &metadata : edit.add_files) {

    validateMetadata(metadata);

    const bool already_exists =
        std::any_of(next_state.begin(), next_state.end(),
                    [&](const SstableMetadata &existing) {
                      return existing.path == metadata.path;
                    });

    if (already_exists) {
      throw std::runtime_error(
          "MANIFEST edit tried to add a duplicate SSTable");
    }

    next_state.push_back(metadata);
  }

  /*
   * Keep the persisted MANIFEST deterministic.
   */
  sortMetadata(next_state);

  // -----------------------------------------------------------------------
  // 3. Advance the recovery checkpoint if this edit carries one
  // -----------------------------------------------------------------------

  if (edit.persisted_through_.has_value()) {

    /*
     * Recovery progress is monotonic.
     *
     * Once Chronos has established that all logical writes through
     * sequence N are represented by authoritative SSTable state,
     * a later MANIFEST must never claim a smaller boundary.
     */
    if (next_checkpoint.has_value() &&
        *edit.persisted_through_ < *next_checkpoint) {
      throw std::runtime_error("MANIFEST checkpoint cannot move backwards");
    }

    next_checkpoint = edit.persisted_through_;
  }

  // -----------------------------------------------------------------------
  // 4. Persist the complete next MANIFEST snapshot
  // -----------------------------------------------------------------------

  /*
   * The new SSTable membership and recovery checkpoint are committed
   * together as one MANIFEST snapshot.
   *
   * If persistence fails, the current in-memory authoritative state
   * remains unchanged.
   */
  persistSnapshot(next_state, next_checkpoint);

  // -----------------------------------------------------------------------
  // 5. Publish the committed state in memory
  // -----------------------------------------------------------------------

  /*
   * The persistent commit succeeded, so the in-memory MANIFEST view may now
   * advance to match what is authoritative on disk.
   */
  live_files_ = std::move(next_state);
  persisted_through_ = next_checkpoint;

  /*
   * Physical SSTable deletion is intentionally separate from MANIFEST edits.
   *
   * MANIFEST membership defines logical truth first. Obsolete or orphaned
   * files may remain physically present until recovery/cleanup handles them.
   */
}

} // namespace kronos