#include "kronos/manifest.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <system_error>
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
constexpr const char *MANIFEST_MAGIC = "KRONOS_MANIFEST_V1";

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
   * No MANIFEST yet means the database currently has no
   * authoritative SSTables.
   */
  if (!std::filesystem::exists(manifest_path_)) {

    live_files_.clear();
    return;
  }

  std::ifstream input(manifest_path_, std::ios::binary);

  if (!input) {
    throw std::runtime_error("Failed to open MANIFEST");
  }

  // Build the replacement state separately.
  std::vector<SstableMetadata> loaded_files;

  // -----------------------------------------------------------------------
  // Validate MANIFEST header
  // -----------------------------------------------------------------------

  std::string magic;

  std::getline(input, magic);

  if (magic != MANIFEST_MAGIC) {
    throw std::runtime_error("Invalid MANIFEST header");
  }

  // -----------------------------------------------------------------------
  // Parse every SSTable record
  // -----------------------------------------------------------------------

  while (true) {

    size_t level;

    /*
     * Reaching EOF here means there are no more records.
     */
    if (!(input >> level)) {
      break;
    }

    std::string path_string;
    std::string smallest_key;
    std::string largest_key;

    /*
     * Record format:
     *
     * [level] [quoted path] [quoted smallest key] [quoted largest key]
     *
     * std::quoted() allows paths/keys containing spaces to survive
     * serialization and parsing correctly.
     */
    if (!(input >> std::quoted(path_string) >> std::quoted(smallest_key) >>
          std::quoted(largest_key))) {

      throw std::runtime_error("Malformed MANIFEST record");
    }

    SstableMetadata metadata{std::filesystem::path(path_string), level,
                             smallest_key, largest_key};

    validateMetadata(metadata);

    /*
     * A physical SSTable path must appear at most once in the
     * authoritative live set.
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
   * If parsing stopped for any reason other than EOF,
   * the MANIFEST is malformed.
   */
  if (!input.eof()) {
    throw std::runtime_error("Malformed MANIFEST");
  }

  sortMetadata(loaded_files);

  /*
   * Only now, after the COMPLETE MANIFEST has successfully loaded,
   * replace the current in-memory authoritative state.
   */
  live_files_ = std::move(loaded_files);
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
    const std::vector<SstableMetadata> &files) const {

  const auto parent = manifest_path_.parent_path();

  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  const auto temp_path = temporaryPath();

  {
    /*
     * Build the complete replacement MANIFEST separately.
     *
     * The current MANIFEST remains untouched while this file
     * is being written.
     */
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);

    if (!output) {
      throw std::runtime_error("Failed to create temporary MANIFEST");
    }

    output << MANIFEST_MAGIC << '\n';

    for (const auto &metadata : files) {

      validateMetadata(metadata);

      output << metadata.level << ' ' << std::quoted(metadata.path.string())
             << ' ' << std::quoted(metadata.smallest_key) << ' '
             << std::quoted(metadata.largest_key) << '\n';
    }

    output.flush();

    if (!output) {
      throw std::runtime_error("Failed to write MANIFEST");
    }
  }

  /*
   * Module 7 logical commit point.
   *
   * Before rename:
   *   old MANIFEST is authoritative.
   *
   * After rename:
   *   new MANIFEST is authoritative.
   *
   * Full power-loss durability requires additional fsync handling
   * for both the file and parent directory. That belongs to Module 9.
   */
  std::error_code ec;

  std::filesystem::rename(temp_path, manifest_path_, ec);

  if (ec) {

    std::filesystem::remove(temp_path);

    throw std::runtime_error("Failed to commit MANIFEST");
  }
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
   * Begin with a complete copy of the current authoritative state.
   *
   * We never construct MANIFEST.tmp from only the edit itself.
   */
  auto next_state = live_files_;

  // -----------------------------------------------------------------------
  // Remove SSTables that should stop being authoritative
  // -----------------------------------------------------------------------

  for (const auto &remove_path : edit.remove_files) {

    const auto old_size = next_state.size();

    next_state.erase(std::remove_if(next_state.begin(), next_state.end(),
                                    [&](const SstableMetadata &metadata) {
                                      return metadata.path == remove_path;
                                    }),
                     next_state.end());

    /*
     * Removing an unknown SSTable usually indicates a bug in
     * higher-level state management, so reject the edit.
     */
    if (next_state.size() == old_size) {
      throw std::runtime_error(
          "MANIFEST edit tried to remove an unknown SSTable");
    }
  }

  // -----------------------------------------------------------------------
  // Add newly authoritative SSTables
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
   * Keep snapshots deterministic before writing them.
   */
  sortMetadata(next_state);

  // -----------------------------------------------------------------------
  // Persist BEFORE exposing the new state
  // -----------------------------------------------------------------------

  persistSnapshot(next_state);

  /*
   * The persistent commit succeeded.
   *
   * Only now should the in-memory view become authoritative.
   */
  live_files_ = std::move(next_state);

  /*
   * Notice what does NOT happen here:
   *
   * We do not physically delete edit.remove_files.
   *
   * MANIFEST membership determines logical truth first.
   * Cleanup of obsolete/orphan SSTables is a separate responsibility
   * and will be handled as part of recovery/cleanup work in Module 9.
   */
}

} // namespace kronos