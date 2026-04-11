/*
 * jaim - Sandboxing tool for AI CLI access
 * Copyright (C) 2026 David Mazieres
 * Copyright (C) 2026 Joseph Presley
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Modified 2026 by Joseph Presley: port from Linux to macOS arm64.
 */

#include "fs.h"
#include "defer.h"

#include <charconv>
#include <cstring>
#include <filesystem>

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

bool
glob(std::string_view pattern, std::string_view target)
{
  if (pattern.empty())
    return target.empty();
  if (pattern.front() == '\\') {
    if ((pattern = pattern.substr(1)).empty())
      return false;
  }
  else if (pattern.front() == '*')
    return glob(pattern.substr(1), target) ||
           (!target.empty() && glob(pattern, target.substr(1)));
  return !target.empty() && pattern.front() == target.front() &&
         glob(pattern.substr(1), target.substr(1));
}

std::string
do_fdpath_must(int fd, bool must)
{
  if (fd < 0 || fd == AT_FDCWD) {
    if (must)
      err("fdpath invalid fd {}", fd);
    return ".";
  }
  // macOS: use F_GETPATH to resolve fd to a path
  char pathbuf[MAXPATHLEN];
  if (fcntl(fd, F_GETPATH, pathbuf) != -1) {
    path res(pathbuf);
    if (must && (!res.is_absolute() || !is_fd_at_path(fd, -1, res)))
      err("{} not valid complete path for fd {}", res.string(), fd);
    return res;
  }
  if (must) {
    errno = EBADF;
    syserr("fcntl(F_GETPATH) for fd {}", fd);
  }
  return std::format("fd {} [can't determine path]", fd);
}

std::string
fdpath(int fd, const path &file)
{
  if (fd < 0 || fd == AT_FDCWD || file.is_absolute())
    return file.empty() ? "." : file.string();
  // macOS: use F_GETPATH
  char pathbuf[MAXPATHLEN];
  path res;
  if (fcntl(fd, F_GETPATH, pathbuf) != -1)
    res = pathbuf;
  else
    res = std::format("fd {} [can't determine path]", fd);
  if (!file.empty())
    res = res / file;
  return res;
}

bool
is_fd_at_path(int targetfd, int dfd, const path &file, FollowLinks follow,
              struct stat *sbout)
{
  struct stat sbtmp, sbpath;
  if (!sbout)
    sbout = &sbtmp;
  if (fstat(targetfd, sbout))
    syserr("fstat({})", fdpath(targetfd));
  if (fstatat(dfd, file.c_str(), &sbpath,
              follow == kFollow ? 0 : AT_SYMLINK_NOFOLLOW))
    return false;
  return sbout->st_dev == sbpath.st_dev && sbout->st_ino == sbpath.st_ino;
}

bool
is_dir_empty(int dirfd)
{
  auto dir = xopendir(dirfd);
  while (auto de = readdir(dir))
    if (de->d_name[0] != '.' ||
        (de->d_name[1] != '\0' &&
         (de->d_name[1] != '.' || de->d_name[2] != '\0')))
      return false;
  return true;
}

Fd
ensure_dir(int dfd, const path &p, mode_t perm, FollowLinks follow,
           bool okay_if_other_owner, std::function<void(int)> createcb)
{
  assert(!p.empty());

  Fd fd;
  bool created = false;
  int flag = follow == kFollow ? 0 : O_NOFOLLOW;
  if (p.is_absolute())
    dfd = *(fd = xopenat(-1, "/", O_RDONLY | O_CLOEXEC));
  for (auto component = p.begin(); component != p.end();) {
    if (Fd nfd = openat(dfd, component->c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | flag)) {
      dfd = *(fd = std::move(nfd));
      ++component;
    }
    else if (errno != ENOENT)
      syserr(R"(ensure_dir("{}"): open("{}"))", p.string(),
             fdpath(dfd, *component));
    else if (created = !mkdirat(dfd, component->c_str(), perm),
             !created && errno != EEXIST)
      syserr(R"(ensure_dir("{}"): mkdir("{}"))", p.string(),
             fdpath(dfd, *component));
    else if (struct stat sb; fstatat(dfd, component->c_str(), &sb, 0))
      syserr(R"(ensure_dir("{}"): stat("{}"))", p.string(),
             fdpath(dfd, *component));
    else if (!S_ISDIR(sb.st_mode)) {
      syserr(R"(ensure_dir("{}"): "{}" is not a directory)", p.string(),
             fdpath(dfd, *component));
    }
    // Don't advance iterator; want to open directory we just created
  }

  auto sb = xfstat(*fd);
  if (!okay_if_other_owner) {
    auto euid = geteuid();
    if (sb.st_uid != euid)
      err("{}: has uid {} should have {}", p.string(), sb.st_uid, euid);
  }
  if (auto m = sb.st_mode & perm; m != (sb.st_mode & 07777) && fchmod(*fd, m))
    syserr(R"(fchmod("{}", {:o}))", p.string(), m);
  if (created)
    createcb(*fd);
  return fd;
}

std::string
open_flags_to_string(int flags)
{
  struct Flag {
    int bits;
    const char *name;
  };
  static constexpr auto composites = std::to_array<Flag>({
      {O_ACCMODE, "3"},
      {O_SYNC, "O_SYNC"},
  });
  static constexpr auto known_flags = std::to_array<Flag>({
      {O_WRONLY, "O_WRONLY"},
      {O_RDWR, "O_RDWR"},
      {O_CREAT, "O_CREAT"},
      {O_EXCL, "O_EXCL"},
      {O_NOCTTY, "O_NOCTTY"},
      {O_TRUNC, "O_TRUNC"},
      {O_APPEND, "O_APPEND"},
      {O_NONBLOCK, "O_NONBLOCK"},
      {O_DSYNC, "O_DSYNC"},
      {O_ASYNC, "O_ASYNC"},
      {O_DIRECTORY, "O_DIRECTORY"},
      {O_NOFOLLOW, "O_NOFOLLOW"},
      {O_CLOEXEC, "O_CLOEXEC"},
      {O_SYNC, "O_SYNC"},
  });

  std::string result;
  auto append = [&](const char *name) {
    result += name;
    result += '|';
  };

  if ((flags & O_ACCMODE) == 0)
    append("O_RDONLY");

  for (auto &c : composites)
    if ((flags & c.bits) == c.bits) {
      append(c.name);
      flags &= ~c.bits;
    }

  for (auto &f : known_flags)
    if (flags & f.bits)
      append(f.name);

  if (auto n = result.size())
    result.resize(n - 1);
  return result;
}

std::string
read_fd(int fd)
{
  std::string ret;
  if (auto sb = xfstat(fd); sb.st_size > 0x100'0000) {
    // Let's not go crazy with sparse files and such
    errno = EFBIG;
    syserr("{}", fdpath(fd));
  }
  else if (sb.st_size > 0)
    ret.reserve(sb.st_size);
  for (;;) {
    char buf[4096];
    auto n = read(fd, buf, sizeof(buf));
    if (n == 0)
      return ret;
    if (n < 0)
      syserr("{}: read", fdpath(fd));
    ret.append(buf, size_t(n));
  }
}

std::expected<std::string, std::system_error>
try_read_file(int dfd, path file)
{
  Fd fdholder;
  int fd = dfd;
  if (!file.empty()) {
    fdholder = openat(fd, file.c_str(), O_RDONLY | O_CLOEXEC);
    if (!fdholder)
      return std::unexpected(
          std::system_error(errno, std::system_category(), fdpath(fd, file)));
    fd = *fdholder;
  }
  return read_fd(fd);
}

Fd
ensure_file(int dfd, path file, std::string_view contents, int mode,
            std::function<void(int)> createcb)
{
  assert(!file.empty());

  if (Fd fd = openat(dfd, file.c_str(), O_RDONLY | O_CLOEXEC)) {
    if (!S_ISREG(xfstat(*fd).st_mode))
      err("{}: not a regular file", fdpath(dfd, file));
    return fd;
  }
  if (errno != ENOENT)
    syserr("{}", fdpath(dfd, file));

  path tmp = cat(file, std::format("~{}~", getpid()));
  unlinkat(dfd, tmp.c_str(), 0);
  Defer cleanup{[dfd, &tmp] { unlinkat(dfd, tmp.c_str(), 0); }};

  Fd fd =
      xopenat(dfd, tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, mode);
  for (size_t i = 0; i < contents.size();) {
    if (auto n = write(*fd, contents.data() + i, contents.size() - i); n < 0)
      syserr(R"(write(tmp for "{}"))", fdpath(dfd, file));
    else
      i += n;
  }
  if (fsync(*fd))
    syserr("fsync(\"{}\")", fdpath(*fd));
  if (renameat(dfd, tmp.c_str(), dfd, file.c_str()))
    syserr(R"(rename("{}" -> "{}") in "{}")", tmp.string(), file.string(),
           fdpath(*fd));
  cleanup.release();
  // have to reopen for reading
  fd = xopenat(dfd, file.c_str(), O_RDONLY | O_CLOEXEC);
  createcb(*fd);
  return fd;
}
