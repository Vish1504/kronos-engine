#pragma once

#include "kronos/types.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace kronos {

/*
 * Manifest
 * --------
 * Stores the authoritative set of SSTables that currently belong
 * to the database.
 *
 * An SSTable merely existing on disk does NOT make it live.
 * Membership in the MANIFEST determines whether Chronos should
 * consider that SSTable part of the database.
 *
 * Example:
 *
 * Disk:
 *   10.sst
 *   11.sst
 *   20.sst
 *
 * MANIFEST:
 *   10.sst
 *   11.sst
 *
 * In this case 20.sst is not authoritative, even though the
 * physical file exists.
 *
 * Module 7 uses a simple full-snapshot MANIFEST:
 *
 *   current live state
 *          ↓
 *   copy + apply edit
 *          ↓
 *   write complete MANIFEST.tmp
 *          ↓
 *   rename to MANIFEST
 *
 * Full power-loss durability and orphan-file cleanup are handled
 * later in Module 9.
 */
class Manifest {

public:
  /*
   * Open the MANIFEST at manifest_path and load its authoritative state.
   *
   * Any leftover MANIFEST.tmp is treated as an uncommitted edit and
   * discarded before loading the authoritative MANIFEST.
   */
  explicit Manifest(std::filesystem::path manifest_path);

  /*
   * Reload the authoritative live-file set from disk.
   *
   * The new state is constructed separately and only replaces
   * live_files_ after the complete MANIFEST has been parsed and validated.
   */
  void load();

  /*
   * Return the complete authoritative live SSTable set.
   */
  const std::vector<SstableMetadata> &liveFiles() const noexcept;

  /*
   * Return all authoritative SSTables belonging to one level.
   *
   * Used by higher-level storage and compaction logic.
   */
  std::vector<SstableMetadata> filesAtLevel(size_t level) const;

  /*
   * Return true if the given SSTable path is currently authoritative.
   *
   * Physical file existence is irrelevant here.
   * This checks MANIFEST membership only.
   */
  bool contains(const std::filesystem::path &path) const;

  /*
   * Commit one logical change to the authoritative live-file set.
   *
   * ManifestEdit describes:
   *
   *   remove_files
   *     SSTables that should stop being authoritative.
   *
   *   add_files
   *     New SSTables that should become authoritative.
   *
   * applyEdit() does NOT physically delete obsolete SSTables.
   * It only changes logical membership.
   *
   * The complete next state is first persisted to MANIFEST.tmp.
   * Only after that succeeds is the temporary file renamed to MANIFEST.
   *
   * Full fsync / directory durability is deferred to Module 9.
   */
  void applyEdit(const ManifestEdit &edit);

private:
  /*
   * Path of the authoritative MANIFEST file.
   */
  std::filesystem::path manifest_path_;

  /*
   * In-memory representation of the currently authoritative
   * SSTable set.
   */
  std::vector<SstableMetadata> live_files_;

  /*
   * Return the temporary path used while constructing
   * the next MANIFEST snapshot.
   *
   * Example:
   *
   *   MANIFEST
   *   MANIFEST.tmp
   */
  std::filesystem::path temporaryPath() const;

  /*
   * Persist a COMPLETE MANIFEST snapshot.
   *
   * 'files' is not merely a list of changes.
   * It represents the entire authoritative state that should exist
   * after the commit.
   *
   * The snapshot is written to MANIFEST.tmp and then renamed
   * to MANIFEST.
   */
  void persistSnapshot(const std::vector<SstableMetadata> &files) const;
};

} // namespace kronos