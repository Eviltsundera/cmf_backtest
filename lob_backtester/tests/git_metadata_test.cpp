#include "lob/utils/GitMetadata.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {

std::filesystem::path unique_temp_dir(const std::string &name) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / (name + "_" + std::to_string(now));
}

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::ofstream out(path);
  out << text;
}

} // namespace

TEST(GitMetadataTest, ReadsLinkedWorktreeLooseRefsFromCommonGitDir) {
  const std::filesystem::path root = unique_temp_dir("lob_git_metadata_loose_ref");
  const std::filesystem::path common_git_dir = root / "common.git";
  const std::filesystem::path worktree = root / "linked";
  const std::filesystem::path git_dir = common_git_dir / "worktrees" / "linked";
  const std::string commit = "0123456789abcdef0123456789abcdef01234567";

  std::filesystem::create_directories(common_git_dir / "refs" / "heads");
  std::filesystem::create_directories(git_dir);
  std::filesystem::create_directories(worktree);
  write_text(worktree / ".git", "gitdir: ../common.git/worktrees/linked\n");
  write_text(git_dir / "HEAD", "ref: refs/heads/main\n");
  write_text(git_dir / "commondir", "../..\n");
  write_text(common_git_dir / "refs" / "heads" / "main", commit + "\n");
  write_text(worktree / "config.yaml", "run: {}\n");

  const std::optional<std::string> resolved = lob::utils::find_git_commit(worktree / "config.yaml");

  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, commit);
  std::filesystem::remove_all(root);
}

TEST(GitMetadataTest, ReadsLinkedWorktreePackedRefsFromCommonGitDir) {
  const std::filesystem::path root = unique_temp_dir("lob_git_metadata_packed_ref");
  const std::filesystem::path common_git_dir = root / "common.git";
  const std::filesystem::path worktree = root / "linked";
  const std::filesystem::path git_dir = common_git_dir / "worktrees" / "linked";
  const std::string commit = "fedcba9876543210fedcba9876543210fedcba98";

  std::filesystem::create_directories(common_git_dir);
  std::filesystem::create_directories(git_dir);
  std::filesystem::create_directories(worktree);
  write_text(worktree / ".git", "gitdir: ../common.git/worktrees/linked\n");
  write_text(git_dir / "HEAD", "ref: refs/heads/packed\n");
  write_text(git_dir / "commondir", "../..\n");
  write_text(common_git_dir / "packed-refs",
             "# pack-refs with: peeled fully-peeled sorted\n" + commit + " refs/heads/packed\n");
  write_text(worktree / "config.yaml", "run: {}\n");

  const std::optional<std::string> resolved = lob::utils::find_git_commit(worktree / "config.yaml");

  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, commit);
  std::filesystem::remove_all(root);
}
