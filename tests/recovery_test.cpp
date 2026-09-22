#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <kronos/config.hpp>
#include <kronos/engine.hpp>
#include <kronos/manifest.hpp>

using namespace kronos;

namespace {

const std::filesystem::path DB_PATH = "recovery_test_db";

const std::filesystem::path MISSING_SST_DB_PATH = "recovery_missing_sst_db";

const std::filesystem::path CONFIG_PATH = "recovery_test.conf";

void writeTestConfig() {
  std::ofstream config_file(CONFIG_PATH);

  if (!config_file) {
    throw std::runtime_error("Could not create recovery test config");
  }

  /*
   * Large Memtable:
   *
   * crash-mode writes should remain in RAM and WAL rather than
   * triggering a normal size-based flush before _Exit().
   */
  config_file << "memtable_size_mb=64\n";
  config_file << "bloom_filter_enabled=true\n";
  config_file << "log_path=recovery_test.log\n";
  config_file << "l0_compaction_trigger=4\n";
}

bool expectFound(KronosEngine &db, const std::string &key,
                 const std::string &expected) {
  const auto result = db.get(key);

  if (result.status != GetStatus::FOUND || result.value != expected) {

    std::cerr << "FAIL: expected " << key << " = " << expected << '\n';

    return false;
  }

  return true;
}

bool expectDeleted(KronosEngine &db, const std::string &key) {
  const auto result = db.get(key);

  if (result.status != GetStatus::DELETED) {
    std::cerr << "FAIL: expected tombstone for " << key << '\n';

    return false;
  }

  return true;
}

bool verifyRecoveredState(KronosEngine &db) {
  if (!expectFound(db, "persisted", "from-sstable")) {
    return false;
  }

  if (!expectFound(db, "wal_only", "survived-crash")) {
    return false;
  }

  if (!expectFound(db, "updated", "new-value")) {
    return false;
  }

  if (!expectDeleted(db, "deleted_after_checkpoint")) {
    return false;
  }

  return true;
}

int runMissingSstableTest(const Config &config) {
  std::filesystem::remove_all(MISSING_SST_DB_PATH);

  /*
   * Create one legitimate authoritative SSTable.
   */
  {
    KronosEngine db(config, MISSING_SST_DB_PATH);

    db.put("must-survive", "authoritative-value");

    db.shutdown();
  }

  Manifest manifest(MISSING_SST_DB_PATH / "MANIFEST");

  if (manifest.liveFiles().empty()) {
    std::cerr << "FAIL: missing-SST test created no live SSTable\n";

    return 1;
  }

  /*
   * Delete a file MANIFEST explicitly says is authoritative.
   */
  const auto referenced_path = manifest.liveFiles().front().path;

  if (!std::filesystem::exists(referenced_path)) {

    std::cerr << "FAIL: referenced SSTable did not exist before deletion\n";

    return 1;
  }

  std::filesystem::remove(referenced_path);

  bool open_failed = false;

  try {
    KronosEngine broken(config, MISSING_SST_DB_PATH);

    /*
     * If construction succeeds, recovery incorrectly accepted a
     * database whose authoritative state is physically incomplete.
     */
  } catch (const std::exception &) {
    open_failed = true;
  }

  if (!open_failed) {
    std::cerr << "FAIL: engine opened with a missing "
                 "MANIFEST-referenced SSTable\n";

    return 1;
  }

  std::filesystem::remove_all(MISSING_SST_DB_PATH);

  std::cout << "PASS: missing authoritative SSTable fails open\n";

  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: recovery_test "
                 "<seed|crash|verify|missing-sst>\n";

    return 1;
  }

  const std::string mode = argv[1];

  try {
    writeTestConfig();

    Config config(CONFIG_PATH);

    // =====================================================
    // SEED
    // =====================================================

    if (mode == "seed") {
      std::filesystem::remove_all(DB_PATH);

      {
        KronosEngine db(config, DB_PATH);

        /*
         * Sequences:
         *
         * 0 -> persisted
         * 1 -> updated old-value
         */
        db.put("persisted", "from-sstable");

        db.put("updated", "old-value");

        /*
         * Graceful shutdown:
         *
         * active Memtable
         *      ↓
         * immutable
         *      ↓
         * durable SSTable
         *      ↓
         * MANIFEST commit + checkpoint
         *      ↓
         * WAL reclamation
         */
        db.shutdown();
      }

      /*
       * Create a physically present but completely invalid SSTable-looking
       * file which is NOT referenced by MANIFEST.
       *
       * Recovery must ignore it because directory contents do not define
       * logical database membership.
       */
      const auto orphan_path = DB_PATH / "orphan-unreferenced.sst";

      {
        std::ofstream orphan(orphan_path, std::ios::binary);

        orphan << "this is intentionally not a valid SSTable";
      }

      std::cout << "PASS: seed state flushed and checkpointed\n";

      std::cout << "PASS: unreferenced orphan prepared for recovery test\n";

      return 0;
    }

    // =====================================================
    // CRASH
    // =====================================================

    if (mode == "crash") {
      /*
       * Opening successfully here already proves that the invalid physical
       * orphan from seed mode was ignored because MANIFEST does not reference
       * it.
       */
      KronosEngine db(config, DB_PATH);

      /*
       * These writes occur after the MANIFEST checkpoint.
       *
       * They therefore exist only in:
       *
       *   WAL
       *   active Memtable
       *
       * when _Exit() kills the process.
       */

      db.put("wal_only", "survived-crash");

      db.put("updated", "new-value");

      db.put("deleted_after_checkpoint", "temporary");

      db.remove("deleted_after_checkpoint");

      std::cout << "New WAL-only writes completed. "
                   "Simulating crash.\n"
                << std::flush;

      /*
       * No destructors.
       * No graceful shutdown.
       * No final Memtable flush.
       */
      std::_Exit(0);
    }

    // =====================================================
    // VERIFY
    // =====================================================

    if (mode == "verify") {
      const auto orphan_path = DB_PATH / "orphan-unreferenced.sst";

      if (!std::filesystem::exists(orphan_path)) {

        std::cerr << "FAIL: orphan test file unexpectedly disappeared\n";

        return 1;
      }

      /*
       * First recovery:
       *
       * checkpointed state comes from SSTables,
       * post-checkpoint state comes from WAL.
       */
      {
        KronosEngine db(config, DB_PATH);

        if (!verifyRecoveredState(db)) {
          return 1;
        }

        std::cout << "PASS: MANIFEST checkpoint + WAL recovery\n";

        std::cout << "PASS: unreferenced orphan ignored during startup\n";

        /*
         * This graceful shutdown persists the recovered Memtable and allows
         * WAL reclamation to run.
         */
        db.shutdown();
      }

      /*
       * Recovery-of-recovery:
       *
       * Open the database AGAIN after the first recovery completed and
       * shutdown persisted the recovered state.
       *
       * We should see exactly the same logical database state.
       */
      {
        KronosEngine db(config, DB_PATH);

        if (!verifyRecoveredState(db)) {
          std::cerr << "FAIL: repeated recovery changed database state\n";

          return 1;
        }

        db.shutdown();
      }

      std::cout << "PASS: repeated recovery is stable\n";

      return 0;
    }

    // =====================================================
    // MISSING AUTHORITATIVE SSTABLE
    // =====================================================

    if (mode == "missing-sst") {
      return runMissingSstableTest(config);
    }

    std::cerr << "Unknown mode: " << mode << '\n';

    return 1;

  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';

    return 1;
  }
}