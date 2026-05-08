#include "lob/utils/GitMetadata.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace lob::utils {
namespace {

std::optional<std::string> read_first_line(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  std::string line;
  std::getline(in, line);
  if (line.empty()) {
    return std::nullopt;
  }
  return line;
}

std::string trim_copy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::optional<std::filesystem::path> git_dir_from_root(const std::filesystem::path &root) {
  const std::filesystem::path marker = root / ".git";
  std::error_code error;
  if (std::filesystem::is_directory(marker, error)) {
    return marker;
  }
  if (std::filesystem::is_regular_file(marker, error)) {
    const std::optional<std::string> line = read_first_line(marker);
    constexpr std::string_view prefix = "gitdir:";
    if (line && line->starts_with(prefix)) {
      std::filesystem::path git_dir = trim_copy(line->substr(prefix.size()));
      if (git_dir.is_relative()) {
        git_dir = root / git_dir;
      }
      return git_dir.lexically_normal();
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> find_git_dir(std::filesystem::path start) {
  std::error_code error;
  if (std::filesystem::is_regular_file(start, error)) {
    start = start.parent_path();
  }
  start = std::filesystem::absolute(start, error);
  if (error) {
    return std::nullopt;
  }

  for (;;) {
    if (const std::optional<std::filesystem::path> git_dir = git_dir_from_root(start)) {
      return git_dir;
    }
    if (!start.has_parent_path() || start == start.parent_path()) {
      return std::nullopt;
    }
    start = start.parent_path();
  }
}

std::filesystem::path common_git_dir_for(const std::filesystem::path &git_dir) {
  const std::optional<std::string> common_dir_line = read_first_line(git_dir / "commondir");
  if (!common_dir_line) {
    return git_dir;
  }

  std::filesystem::path common_dir = trim_copy(*common_dir_line);
  if (common_dir.is_relative()) {
    common_dir = git_dir / common_dir;
  }
  return common_dir.lexically_normal();
}

std::optional<std::string> read_packed_ref(const std::filesystem::path &git_dir,
                                           const std::string_view ref) {
  std::ifstream in(git_dir / "packed-refs");
  if (!in) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() == '#' || line.front() == '^') {
      continue;
    }
    const std::size_t separator = line.find(' ');
    if (separator == std::string::npos) {
      continue;
    }
    if (std::string_view(line).substr(separator + 1) == ref) {
      return line.substr(0, separator);
    }
  }
  return std::nullopt;
}

std::optional<std::string> read_ref(const std::filesystem::path &git_dir,
                                    const std::filesystem::path &common_git_dir,
                                    const std::string_view ref) {
  if (const std::optional<std::string> commit = read_first_line(git_dir / ref)) {
    return commit;
  }
  if (common_git_dir != git_dir) {
    if (const std::optional<std::string> commit = read_first_line(common_git_dir / ref)) {
      return commit;
    }
  }
  if (const std::optional<std::string> commit = read_packed_ref(git_dir, ref)) {
    return commit;
  }
  if (common_git_dir != git_dir) {
    return read_packed_ref(common_git_dir, ref);
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> find_git_commit(const std::filesystem::path &start) {
  const std::optional<std::filesystem::path> git_dir = find_git_dir(start);
  if (!git_dir) {
    return std::nullopt;
  }

  const std::optional<std::string> head = read_first_line(*git_dir / "HEAD");
  if (!head) {
    return std::nullopt;
  }

  constexpr std::string_view ref_prefix = "ref: ";
  if (!head->starts_with(ref_prefix)) {
    return *head;
  }

  const std::string ref = trim_copy(head->substr(ref_prefix.size()));
  return read_ref(*git_dir, common_git_dir_for(*git_dir), ref);
}

} // namespace lob::utils
