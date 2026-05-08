#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lob::utils {

std::optional<std::string> find_git_commit(const std::filesystem::path &start);

} // namespace lob::utils
