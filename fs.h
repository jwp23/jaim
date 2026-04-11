// -*-C++-*-

#pragma once

#include "defer.h"
#include "err.h"

#include <algorithm>
#include <cassert>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Self-closing file descriptor
using Fd = RaiiHelper<::close, int, -1>;

using std::filesystem::path;

inline path
cat(path left, const path &right)
{
  return left += right;
}

inline size_t
components(const path &p)
{
  return std::ranges::distance(p.begin(), p.end());
}

inline bool
contains(const path &dir, const path &subpath)
{
  return std::ranges::mismatch(dir, subpath).in1 == dir.end();
}

// True if target matches pattern (with * expanded)
bool glob(std::string_view pattern, std::string_view target);

// Compare paths component by component so subtrees are contiguous
struct PathLess {
  static bool operator()(const path &a, const path &b)
  {
    return std::ranges::lexicographical_compare(a, b);
  }
};

using PathSet = std::set<path, PathLess>;
using PathMultiset = std::multiset<path, PathLess>;
template<typename V> using PathMap = std::map<path, V, PathLess>;

std::string fdpath(int fd, const path &file);

inline std::string
fdpath(int fd, std::same_as<bool> auto must)
{
  extern std::string do_fdpath_must(int fd, bool must);
  return do_fdpath_must(fd, must);
}
inline std::string
fdpath(int fd)
{
  return fdpath(fd, false);
}

enum class FollowLinks {
  kNoFollow = 0,
  kFollow = 1,
};
using enum FollowLinks;

// Conservatively fails if file is not a regular file or cannot be
// statted for any reason.
bool is_fd_at_path(int targetfd, int dfd, const path &file,
                   FollowLinks follow = kNoFollow,
                   struct stat *sbout = nullptr);

bool is_dir_empty(int dirfd);

using CreateCB = std::function<void(int)>;

Fd ensure_dir(
    int dfd, const path &p, mode_t perm, FollowLinks follow,
    bool okay_if_other_owner = false, CreateCB createcb = [](int) {});

std::string open_flags_to_string(int flags);

inline Fd
xopenat(int dfd, const path &file, int flags, mode_t mode = 0644)
{
  if (int fd = openat(dfd, file.c_str(), flags, mode); fd >= 0)
    return fd;
  syserr(R"(openat("{}", {}))",
         dfd >= 0 ? (fdpath(dfd) / file).string() : file.string(),
         open_flags_to_string(flags));
}

inline Fd
xdup(int fd, int minfd = 3)
{
  auto ret = fcntl(fd, F_DUPFD_CLOEXEC, minfd);
  if (ret == -1)
    syserr("{}: F_DUPFD_CLOEXEC", fdpath(fd));
  return ret;
}

inline std::expected<RaiiHelper<closedir>, std::system_error>
try_opendir(int dfd, path file = {}, FollowLinks follow = kNoFollow)
{
  if (file.empty())
    // re-open in case dfd is O_PATH and to avoid messing with the
    // offset of dfd if we read the directory multiple times
    file = ".";
  Fd fd = openat(dfd, file.c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                     (follow == kNoFollow ? O_NOFOLLOW : 0));
  if (!fd)
    return std::unexpected{
        std::system_error(errno, std::system_category(), fdpath(dfd, file))};

  if (auto d = fdopendir(*fd)) {
    fd.release();
    return d;
  }
  return std::unexpected{
      std::system_error(errno, std::system_category(),
                        std::format("{}: fdopendir", fdpath(*fd)))};
}

inline RaiiHelper<closedir>
xopendir(int dfd, path file = {}, FollowLinks follow = kNoFollow)
{
  if (auto r = try_opendir(dfd, file, follow))
    return std::move(*r);
  else
    throw r.error();
}

// dirent::d_name is an array, so won't convert properly to types that
// treat a char array differently from a const char *.
inline const char *
d_name(const struct dirent *de)
{
  return de->d_name;
}

inline std::array<Fd, 2>
xpipe()
{
  int fds[2];
  if (pipe(fds))
    syserr("pipe");
  // Set close-on-exec for both ends
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return {fds[0], fds[1]};
}

inline struct stat
xfstat(int fd, path file = {}, FollowLinks follow = kFollow)
{
  struct stat sb;
  if (file.empty()) {
    if (fstat(fd, &sb))
      syserr(R"(fstat("{}"))", fdpath(fd));
  }
  else if (fstatat(fd, file.c_str(), &sb,
                   follow == kFollow ? 0 : AT_SYMLINK_NOFOLLOW))
    syserr(R"({}stat("{}"))", follow == kFollow ? "" : "l", fdpath(fd, file));
  return sb;
}

std::string read_fd(int fd);

// This tries to read a file.  It will return an error if the file
// cannot be opened (e.g., because it does not exist), but could still
// throw if reading the actual file returns an error or allocating the
// buffer exhausts memory.
std::expected<std::string, std::system_error> try_read_file(int dfd,
                                                            path file = {});

inline std::string
read_file(int dfd, path file = {})
{
  if (auto res = try_read_file(dfd, file))
    return std::move(*res);
  else
    throw res.error();
}

inline void
create_warn(int fd)
{
  warn("created {}", fdpath(fd));
}

Fd ensure_file(
    int dfd, path file, std::string_view contents, int mode = 0600,
    CreateCB createcb = [](int) {});
