#include "kronos/manifest.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace kronos;

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

template <typename Fn> void expectThrows(Fn &&fn, const std::string &message) {
  bool threw = false;

  try {
    fn();
  } catch (const std::exception &) {
    threw = true;
  }

  expect(threw, message);
}

void writeTextFile(const std::filesystem::path &path,
                   const std::string &contents) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);

  if (!output) {
    throw std::runtime_error("Could not create test MANIFEST");
  }

  output << contents;

  if (!output) {
    throw std::runtime_error("Could not write test MANIFEST");
  }
}

void testCheckpointNoneAndZero() {
  const std::filesystem::path test_dir = "manifest_checkpoint_encoding_test";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto manifest_path = test_dir / "MANIFEST";

  Manifest manifest(manifest_path);

  /*
   * Brand-new database:
   *
   * no checkpoint has ever been committed.
   */
  expect(!manifest.persistedThrough().has_value(),
         "New MANIFEST should have no checkpoint");

  /*
   * Force an empty snapshot to disk.
   *
   * It should serialize:
   *
   * CHECKPOINT NONE
   */
  ManifestEdit none_edit;
  manifest.applyEdit(none_edit);

  Manifest none_reloaded(manifest_path);

  expect(!none_reloaded.persistedThrough().has_value(),
         "CHECKPOINT NONE must reload as no checkpoint");

  /*
   * Sequence zero is a real sequence number.
   *
   * Therefore:
   *
   * NONE != 0
   */
  ManifestEdit zero_edit;
  zero_edit.persisted_through_ = 0;

  none_reloaded.applyEdit(zero_edit);

  Manifest zero_reloaded(manifest_path);

  expect(zero_reloaded.persistedThrough().has_value(),
         "CHECKPOINT 0 must be present");

  expect(*zero_reloaded.persistedThrough() == 0,
         "CHECKPOINT 0 must reload as sequence zero");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: CHECKPOINT NONE and CHECKPOINT 0 remain distinct\n";
}

void testManifestMembershipAndCommitBoundary() {
  const std::filesystem::path test_dir = "manifest_membership_test";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto manifest_path = test_dir / "MANIFEST";

  const auto old0 = test_dir / "10.sst";

  const auto old1 = test_dir / "11.sst";

  const auto output = test_dir / "20.sst";

  Manifest manifest(manifest_path);

  expect(manifest.liveFiles().empty(), "New MANIFEST should start empty");

  // -------------------------------------------------------
  // Commit initial authoritative state
  // -------------------------------------------------------

  ManifestEdit initial_edit;

  initial_edit.add_files = {{old0, 0, "A", "D"}, {old1, 0, "E", "H"}};

  initial_edit.persisted_through_ = 10;

  manifest.applyEdit(initial_edit);

  expect(manifest.contains(old0), "MANIFEST should contain 10.sst");

  expect(manifest.contains(old1), "MANIFEST should contain 11.sst");

  expect(manifest.persistedThrough().has_value() &&
             *manifest.persistedThrough() == 10,
         "Initial checkpoint should be 10");

  // -------------------------------------------------------
  // Simulate crash BEFORE MANIFEST commit
  // -------------------------------------------------------

  /*
   * Compactor/SSTable builder produced an output file,
   * but MANIFEST never committed it.
   */
  {
    std::ofstream orphan(output);
    orphan << "uncommitted output";
  }

  /*
   * Also simulate a MANIFEST.tmp that was being prepared
   * when the process died.
   *
   * Constructor recovery must discard MANIFEST.tmp and load
   * only the authoritative MANIFEST.
   */
  const auto temp_path = std::filesystem::path(manifest_path.string() + ".tmp");

  writeTextFile(temp_path, "THIS SNAPSHOT NEVER COMMITTED\n");

  Manifest before_commit(manifest_path);

  expect(!std::filesystem::exists(temp_path),
         "Leftover MANIFEST.tmp should be discarded");

  expect(before_commit.contains(old0),
         "Old SSTable must remain authoritative before commit");

  expect(before_commit.contains(old1),
         "Old SSTable must remain authoritative before commit");

  expect(!before_commit.contains(output),
         "Uncommitted output must not become authoritative");

  expect(before_commit.persistedThrough().has_value() &&
             *before_commit.persistedThrough() == 10,
         "Uncommitted MANIFEST.tmp must not change checkpoint");

  // -------------------------------------------------------
  // Commit compaction transition
  // -------------------------------------------------------

  ManifestEdit compaction_edit;

  compaction_edit.remove_files = {old0, old1};

  compaction_edit.add_files = {{output, 1, "A", "H"}};

  /*
   * Compaction does NOT advance recovery coverage.
   *
   * No persisted_through_ is supplied here.
   */
  before_commit.applyEdit(compaction_edit);

  expect(!before_commit.contains(old0),
         "10.sst should become obsolete after commit");

  expect(!before_commit.contains(old1),
         "11.sst should become obsolete after commit");

  expect(before_commit.contains(output),
         "20.sst should become authoritative after commit");

  expect(before_commit.persistedThrough().has_value() &&
             *before_commit.persistedThrough() == 10,
         "Compaction edit must preserve checkpoint");

  // -------------------------------------------------------
  // Reload committed state
  // -------------------------------------------------------

  Manifest after_commit(manifest_path);

  expect(after_commit.liveFiles().size() == 1,
         "Committed MANIFEST should contain one SSTable");

  expect(after_commit.contains(output),
         "Committed output should survive reload");

  expect(after_commit.persistedThrough().has_value() &&
             *after_commit.persistedThrough() == 10,
         "Checkpoint must survive committed compaction reload");

  // -------------------------------------------------------
  // Zero-output compaction
  // -------------------------------------------------------

  ManifestEdit zero_output_edit;

  zero_output_edit.remove_files = {output};

  after_commit.applyEdit(zero_output_edit);

  expect(after_commit.liveFiles().empty(),
         "MANIFEST should support zero-output compaction");

  expect(after_commit.persistedThrough().has_value() &&
             *after_commit.persistedThrough() == 10,
         "Zero-output compaction must preserve checkpoint");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: MANIFEST commit boundary preserves authoritative state\n";

  std::cout << "PASS: edits without checkpoints preserve recovery checkpoint\n";

  std::cout << "PASS: zero-output compaction preserves checkpoint\n";
}

void testCheckpointCannotMoveBackward() {
  const std::filesystem::path test_dir = "manifest_checkpoint_monotonic_test";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto manifest_path = test_dir / "MANIFEST";

  Manifest manifest(manifest_path);

  ManifestEdit first;
  first.persisted_through_ = 100;

  manifest.applyEdit(first);

  expect(manifest.persistedThrough().has_value() &&
             *manifest.persistedThrough() == 100,
         "Checkpoint should advance to 100");

  ManifestEdit regression;

  regression.add_files.push_back(
      {test_dir / "should-not-commit.sst", 0, "A", "Z"});

  regression.persisted_through_ = 99;

  expectThrows([&] { manifest.applyEdit(regression); },
               "Checkpoint regression must be rejected");

  /*
   * A rejected edit must change neither the checkpoint
   * nor the live-file membership.
   */
  expect(manifest.persistedThrough().has_value() &&
             *manifest.persistedThrough() == 100,
         "Rejected checkpoint regression changed in-memory checkpoint");

  expect(!manifest.contains(test_dir / "should-not-commit.sst"),
         "Rejected edit changed MANIFEST membership");

  Manifest reloaded(manifest_path);

  expect(reloaded.persistedThrough().has_value() &&
             *reloaded.persistedThrough() == 100,
         "Rejected checkpoint regression changed persistent checkpoint");

  expect(!reloaded.contains(test_dir / "should-not-commit.sst"),
         "Rejected edit changed persistent membership");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: MANIFEST checkpoint cannot move backwards\n";
}

void testMalformedManifestRejected() {
  const std::filesystem::path test_dir = "manifest_corruption_test";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto manifest_path = test_dir / "MANIFEST";

  // -------------------------------------------------------
  // Non-numeric checkpoint
  // -------------------------------------------------------

  writeTextFile(manifest_path, "KRONOS_MANIFEST_V2\n"
                               "CHECKPOINT banana\n");

  expectThrows([&] { Manifest malformed(manifest_path); },
               "Non-numeric MANIFEST checkpoint must be rejected");

  // -------------------------------------------------------
  // Numeric prefix + garbage
  // -------------------------------------------------------

  writeTextFile(manifest_path, "KRONOS_MANIFEST_V2\n"
                               "CHECKPOINT 100abc\n");

  expectThrows([&] { Manifest malformed(manifest_path); },
               "Checkpoint with trailing garbage must be rejected");

  // -------------------------------------------------------
  // Negative checkpoint
  // -------------------------------------------------------

  writeTextFile(manifest_path, "KRONOS_MANIFEST_V2\n"
                               "CHECKPOINT -1\n");

  expectThrows([&] { Manifest malformed(manifest_path); },
               "Negative MANIFEST checkpoint must be rejected");

  // -------------------------------------------------------
  // Out-of-range uint64
  // -------------------------------------------------------

  writeTextFile(manifest_path, "KRONOS_MANIFEST_V2\n"
                               "CHECKPOINT 18446744073709551616\n");

  expectThrows([&] { Manifest malformed(manifest_path); },
               "Out-of-range MANIFEST checkpoint must be rejected");

  // -------------------------------------------------------
  // Old/incompatible format
  // -------------------------------------------------------

  writeTextFile(manifest_path, "KRONOS_MANIFEST_V1\n"
                               "CHECKPOINT NONE\n");

  expectThrows([&] { Manifest old_version(manifest_path); },
               "Old MANIFEST format must be rejected");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: malformed MANIFEST checkpoints rejected\n";

  std::cout << "PASS: incompatible MANIFEST version rejected\n";
}

} // namespace

int main() {
  std::cout << "Running MANIFEST tests...\n\n";

  testCheckpointNoneAndZero();
  testManifestMembershipAndCommitBoundary();
  testCheckpointCannotMoveBackward();
  testMalformedManifestRejected();

  std::cout << "\nAll MANIFEST tests passed.\n";

  return 0;
}