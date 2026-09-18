#include "kronos/manifest.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace kronos {

namespace {

constexpr const char *MANIFEST_MAGIC = "KRONOS_MANIFEST_V1";

void sortMetadata(std::vector<SstableMetadata> &files) {

  std::sort(files.begin(), files.end(),
            [](const SstableMetadata &a, const SstableMetadata &b) {
              if (a.level != b.level) {
                return a.level < b.level;
              }

              return a.path.string() < b.path.string();
            });
}

void validateMetadata(const SstableMetadata &metadata) {

  if (metadata.path.empty()) {
    throw std::invalid_argument("SSTable metadata path cannot be empty");
  }

  if (metadata.smallest_key > metadata.largest_key) {

    throw std::invalid_argument("Invalid SSTable key range");
  }
}

} // namespace

Manifest::Manifest(std::filesystem::path manifest_path)
    : manifest_path_(std::move(manifest_path)) {

  // A leftover .tmp file represents an edit that never
  // became authoritative.
  std::error_code ec;

  std::filesystem::remove(temporaryPath(), ec);

  load();
}

std::filesystem::path Manifest::temporaryPath() const {

  return std::filesystem::path(manifest_path_.string() + ".tmp");
}

const std::vector<SstableMetadata> &Manifest::liveFiles() const noexcept {

  return live_files_;
}

std::vector<SstableMetadata> Manifest::filesAtLevel(size_t level) const {

  std::vector<SstableMetadata> result;

  for (const auto &file : live_files_) {

    if (file.level == level) {
      result.push_back(file);
    }
  }

  return result;
}

bool Manifest::contains(const std::filesystem::path &path) const {

  return std::any_of(
      live_files_.begin(), live_files_.end(),
      [&](const SstableMetadata &metadata) { return metadata.path == path; });
}

void Manifest::load() {

  live_files_.clear();

  if (!std::filesystem::exists(manifest_path_)) {
    return;
  }

  std::ifstream input(manifest_path_, std::ios::binary);

  if (!input) {
    throw std::runtime_error("Failed to open MANIFEST");
  }

  std::string magic;

  std::getline(input, magic);

  if (magic != MANIFEST_MAGIC) {
    throw std::runtime_error("Invalid MANIFEST header");
  }

  while (true) {

    size_t level;

    if (!(input >> level)) {
      break;
    }

    std::string path_string;
    std::string smallest_key;
    std::string largest_key;

    if (!(input >> std::quoted(path_string) >> std::quoted(smallest_key) >>
          std::quoted(largest_key))) {

      throw std::runtime_error("Malformed MANIFEST record");
    }

    SstableMetadata metadata{std::filesystem::path(path_string), level,
                             smallest_key, largest_key};

    validateMetadata(metadata);

    if (contains(metadata.path)) {
      throw std::runtime_error("Duplicate SSTable in MANIFEST");
    }

    live_files_.push_back(std::move(metadata));
  }

  if (!input.eof()) {
    throw std::runtime_error("Malformed MANIFEST");
  }

  sortMetadata(live_files_);
}

void Manifest::persistSnapshot(
    const std::vector<SstableMetadata> &files) const {

  const auto parent = manifest_path_.parent_path();

  if (!parent.empty()) {

    std::filesystem::create_directories(parent);
  }

  const auto temp_path = temporaryPath();

  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);

    if (!output) {
      throw std::runtime_error("Failed to create temporary MANIFEST");
    }

    output << MANIFEST_MAGIC << '\n';

    for (const auto &metadata : files) {

      output << metadata.level << ' ' << std::quoted(metadata.path.string())
             << ' ' << std::quoted(metadata.smallest_key) << ' '
             << std::quoted(metadata.largest_key) << '\n';
    }

    output.flush();

    if (!output) {
      throw std::runtime_error("Failed to write MANIFEST");
    }
  }

  // The rename is the logical commit point for Module 7.
  //
  // Full fsync + parent-directory durability is hardened
  // later in Module 9.
  std::error_code ec;

  std::filesystem::rename(temp_path, manifest_path_, ec);

  if (ec) {

    std::filesystem::remove(temp_path);

    throw std::runtime_error("Failed to commit MANIFEST");
  }
}

void Manifest::applyEdit(const ManifestEdit &edit) {

  // Work on a copy first.
  //
  // live_files_ should change only after the persisted
  // MANIFEST commit succeeds.
  auto next_state = live_files_;

  // -------------------------------------------------------
  // Remove obsolete SSTables
  // -------------------------------------------------------

  for (const auto &remove_path : edit.remove_files) {

    const auto old_size = next_state.size();

    next_state.erase(std::remove_if(next_state.begin(), next_state.end(),
                                    [&](const SstableMetadata &metadata) {
                                      return metadata.path == remove_path;
                                    }),
                     next_state.end());

    if (next_state.size() == old_size) {

      throw std::runtime_error("MANIFEST edit tried to remove "
                               "an unknown SSTable");
    }
  }

  // -------------------------------------------------------
  // Add new authoritative SSTables
  // -------------------------------------------------------

  for (const auto &metadata : edit.add_files) {

    validateMetadata(metadata);

    const bool already_exists =
        std::any_of(next_state.begin(), next_state.end(),
                    [&](const SstableMetadata &existing) {
                      return existing.path == metadata.path;
                    });

    if (already_exists) {
      throw std::runtime_error("MANIFEST edit tried to add "
                               "a duplicate SSTable");
    }

    next_state.push_back(metadata);
  }

  sortMetadata(next_state);

  // Persist first.
  persistSnapshot(next_state);

  // Only now does the in-memory view become authoritative.
  live_files_ = std::move(next_state);
}

} // namespace kronos