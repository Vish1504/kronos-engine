#include "kronos/manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace kronos;

namespace {

void expect(bool condition, const std::string &message) {

  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';

    std::exit(1);
  }
}

} // namespace

int main() {

  std::cout << "Running MANIFEST tests...\n\n";

  const std::filesystem::path test_dir = "manifest_test_data";

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  const auto manifest_path = test_dir / "MANIFEST";

  const auto old0 = test_dir / "10.sst";

  const auto old1 = test_dir / "11.sst";

  const auto output = test_dir / "20.sst";

  // -------------------------------------------------------
  // Start with an empty MANIFEST
  // -------------------------------------------------------

  Manifest manifest(manifest_path);

  expect(manifest.liveFiles().empty(), "New MANIFEST should start empty");

  // -------------------------------------------------------
  // Commit initial database state
  // -------------------------------------------------------

  ManifestEdit initial_edit{{}, {{old0, 0, "A", "D"}, {old1, 0, "E", "H"}}};

  manifest.applyEdit(initial_edit);

  expect(manifest.contains(old0), "MANIFEST should contain 10.sst");

  expect(manifest.contains(old1), "MANIFEST should contain 11.sst");

  // -------------------------------------------------------
  // Prove persistence
  // -------------------------------------------------------

  Manifest reloaded(manifest_path);

  expect(reloaded.liveFiles().size() == 2,
         "Reloaded MANIFEST should contain two SSTables");

  expect(reloaded.contains(old0), "Reloaded MANIFEST should contain 10.sst");

  // -------------------------------------------------------
  // Simulate:
  //
  // Compactor produced 20.sst...
  //
  // ...but MANIFEST edit never committed.
  //
  // The existence of the file alone MUST NOT make it
  // authoritative.
  // -------------------------------------------------------

  {
    std::ofstream orphan(output);

    orphan << "uncommitted output";
  }

  Manifest before_commit(manifest_path);

  expect(before_commit.contains(old0),
         "Old SSTables must remain authoritative before commit");

  expect(before_commit.contains(old1),
         "Old SSTables must remain authoritative before commit");

  expect(!before_commit.contains(output),
         "Uncommitted output must not become authoritative");

  // -------------------------------------------------------
  // Commit compaction transition
  // -------------------------------------------------------

  ManifestEdit compaction_edit{{old0, old1}, {{output, 1, "A", "H"}}};

  before_commit.applyEdit(compaction_edit);

  expect(!before_commit.contains(old0),
         "10.sst should become obsolete after MANIFEST commit");

  expect(!before_commit.contains(old1),
         "11.sst should become obsolete after MANIFEST commit");

  expect(before_commit.contains(output),
         "20.sst should become authoritative after commit");

  // -------------------------------------------------------
  // Reload again to prove committed state survived
  // -------------------------------------------------------

  Manifest after_commit(manifest_path);

  expect(after_commit.liveFiles().size() == 1,
         "Committed MANIFEST should contain one SSTable");

  expect(after_commit.contains(output),
         "Committed output should survive MANIFEST reload");

  // -------------------------------------------------------
  // Zero-output compaction:
  //
  // remove an old SSTable and add nothing.
  // -------------------------------------------------------

  ManifestEdit zero_output_edit{{output}, {}};

  after_commit.applyEdit(zero_output_edit);

  expect(after_commit.liveFiles().empty(),
         "MANIFEST should support compaction with no output SSTable");

  std::filesystem::remove_all(test_dir);

  std::cout << "PASS: MANIFEST persists live SSTable state\n";

  std::cout << "PASS: uncommitted output is not authoritative\n";

  std::cout << "PASS: compaction edit replaces old files atomically\n";

  std::cout << "PASS: zero-output compaction is supported\n";

  std::cout << "\nAll MANIFEST tests passed.\n";

  return 0;
}