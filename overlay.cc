/*
 * jaim - Sandboxing tool for AI CLI access
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
 */

// jaim-overlay — copy-on-write overlay filesystem for jaim casual mode.
//
// Two layers:
//
//   lower  read-only.  The user's real $HOME (or any other source tree
//          we want to expose to the sandbox without modification).
//   upper  writable.   $JAIM_CONFIG_DIR/<jail>.changes/ — every write
//          the sandbox makes lands here.  Files copied up from lower
//          appear here on first write.
//
// Conventions match Linux overlayfs / containers/storage's
// fuse-overlayfs so that an upper directory written here could in
// principle be replayed against an overlayfs mount on Linux:
//
//   * Whiteouts use a 0/0 character device in the upper layer (the
//     traditional overlayfs marker).  When mknod of a char device is
//     not permitted (the common case for an unprivileged user on
//     macOS), we fall back to a sidecar regular file named
//     ".wh.<basename>" alongside.  Both forms are recognized on read.
//   * Opaque directories — directories in the upper layer that should
//     hide the corresponding directory in the lower layer entirely —
//     carry a regular file named ".wh..wh..opq" inside them.
//   * Copy-up of a regular file is atomic via a temp file in the
//     same upper directory followed by rename(2).  Mode, owner (where
//     the running uid is permitted), and times are preserved.
//
// We mount on a path the sandbox will see as $HOME.  The casual-mode
// SBPL grants the sandbox read+write access to that mount point and
// keeps the real home denied, so writes can only land in the upper
// layer.  Reads transparently fall through to the lower layer for
// anything the sandbox has not yet modified.
//
// macFUSE 5.2+ provides the libfuse C API on top of an FSKit backend
// (no kernel extension required).  We compile against
// /usr/local/include/fuse and link -lfuse.  If macFUSE is not
// installed at build time, this file is simply omitted from the
// build (see GNUmakefile).  At run time, fuse_main returns a
// non-zero status if the FUSE backend is unavailable; we propagate
// that and the caller (jaim) can surface a useful error.

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <fuse.h>

#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {

// Whiteout / opaque markers.  These names are chosen to interoperate
// with Linux overlayfs and fuse-overlayfs.  A character device 0/0 in
// upper means "the file with the same name in lower has been
// deleted".  Sidecar ".wh.<name>" regulars are a portable fallback we
// honour on read and emit on write when mknod is not permitted.
constexpr const char *WH_PREFIX = ".wh.";
constexpr size_t WH_PREFIX_LEN = 4;
constexpr const char *OPAQUE_MARKER = ".wh..wh..opq";

// Logging.  -d enables FUSE's own debug output; we additionally emit
// our own trace lines to stderr when the user passes -o jdebug.
bool g_debug = false;

void
trace(const char *fmt, ...)
{
  if (!g_debug)
    return;
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[jaim-overlay] ");
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}

// ----------------------------------------------------------------------
// Overlay state and path resolution
// ----------------------------------------------------------------------

struct Overlay {
  // Absolute, canonicalised paths to the two layers.  Both directories
  // must exist before we mount.
  std::string lower;
  std::string upper;

  // Track whether mknod of a char device 0/0 succeeded the first time
  // we tried.  If it returned EPERM we fall back to ".wh.<name>"
  // sidecars for the remainder of this mount.  Sticky across the
  // mount lifetime to keep readdir ordering stable.
  bool can_mknod_whiteout = true;

  // uid/gid we run as — used for chown attempts during copy_up where
  // the source file is owned by someone else.  We never silently
  // change ownership against the user's intent; we simply skip a
  // chown that would fail.
  uid_t my_uid = ::getuid();
  gid_t my_gid = ::getgid();
};

Overlay *
state()
{
  return static_cast<Overlay *>(fuse_get_context()->private_data);
}

// Build "<base>/<rel>" without leaving // in the result.  rel is
// always given to us as a FUSE path beginning with '/'.  An empty
// rel ("") or root ("/") returns base verbatim.
std::string
join(const std::string &base, const char *rel)
{
  if (!rel || !*rel || (rel[0] == '/' && rel[1] == 0))
    return base;
  std::string out;
  out.reserve(base.size() + std::strlen(rel) + 1);
  out = base;
  if (rel[0] != '/')
    out.push_back('/');
  out.append(rel);
  return out;
}

// "/foo/bar/baz" -> {"/foo/bar", "baz"}.  An entry directly under
// root yields {"/", "baz"}.  Root itself yields {"", ""}.  We only
// use this for paths that originate from FUSE, so they always start
// with '/'.
struct Split {
  std::string parent;
  std::string name;
};
Split
split(const char *path)
{
  std::string_view p(path ? path : "");
  if (p == "/" || p.empty())
    return {"", ""};
  auto slash = p.find_last_of('/');
  if (slash == std::string_view::npos)
    return {"", std::string(p)};
  if (slash == 0)
    return {"/", std::string(p.substr(1))};
  return {std::string(p.substr(0, slash)), std::string(p.substr(slash + 1))};
}

// True if a stat result represents an overlayfs whiteout marker (a
// character device with rdev 0).  Linux overlayfs uses major 0 minor
// 0; we honour that and also accept any character device with major
// 0 to be liberal with what we accept.
bool
is_whiteout_dev(const struct stat &sb)
{
  return S_ISCHR(sb.st_mode) && major(sb.st_rdev) == 0 &&
         minor(sb.st_rdev) == 0;
}

// True if the directory at upper/<dir> has an opaque marker file.
// Opaque means "ignore the corresponding dir in lower entirely"; it
// is set by rmdir-then-mkdir and by rename-over-a-lower-dir, where
// the lower's contents must not show through.
bool
is_opaque_dir(const std::string &upper_dir)
{
  std::string m = upper_dir + "/" + OPAQUE_MARKER;
  struct stat sb;
  return ::lstat(m.c_str(), &sb) == 0;
}

// Result of resolving a FUSE path against the layered view.
enum class Where { kNone, kUpper, kLower, kWhiteout };
struct Resolved {
  Where where = Where::kNone;
  std::string path;        // absolute path in whichever layer wins
  struct stat sb;          // populated for kUpper / kLower
};

// Walk the path component by component so that an opaque directory
// at any ancestor level correctly hides the rest of the lower tree
// underneath it.  A whiteout at any ancestor returns kWhiteout
// (which getattr translates to ENOENT) — the entry effectively does
// not exist for the sandbox.
Resolved
resolve(Overlay *o, const char *path)
{
  Resolved r;
  if (!path || !*path) {
    r.where = Where::kNone;
    return r;
  }

  // Special-case root — both layers always have it; upper wins for
  // metadata so chmod/chown of root reflect the user's view.
  if (path[0] == '/' && path[1] == 0) {
    if (::lstat(o->upper.c_str(), &r.sb) == 0) {
      r.where = Where::kUpper;
      r.path = o->upper;
      return r;
    }
    if (::lstat(o->lower.c_str(), &r.sb) == 0) {
      r.where = Where::kLower;
      r.path = o->lower;
      return r;
    }
    return r;
  }

  // Walk the components, tracking whether any ancestor has been
  // marked opaque (so lower fall-through is suppressed below it) or
  // whited out (so the entry is gone).
  bool opaque_above = false;
  std::string cur = "";  // FUSE-style path being built
  std::string up = o->upper;
  std::string lo = o->lower;

  // Iterate over slash-separated components of `path`.
  const char *p = path + 1;  // skip leading '/'
  while (*p) {
    const char *slash = std::strchr(p, '/');
    std::string comp = slash ? std::string(p, slash - p) : std::string(p);
    if (comp.empty())
      break;

    // Whiteout marker file in the parent's upper view kills this
    // entry and everything beneath it.
    {
      std::string wh = up + "/" + std::string(WH_PREFIX) + comp;
      struct stat sb;
      if (::lstat(wh.c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) {
        r.where = Where::kWhiteout;
        return r;
      }
    }

    // Examine this component in upper.
    std::string up_full = up + "/" + comp;
    struct stat up_sb;
    bool in_upper = ::lstat(up_full.c_str(), &up_sb) == 0;
    if (in_upper && is_whiteout_dev(up_sb)) {
      r.where = Where::kWhiteout;
      return r;
    }

    // Examine in lower (only meaningful if no opaque ancestor).
    std::string lo_full = lo + "/" + comp;
    struct stat lo_sb;
    bool in_lower = !opaque_above && ::lstat(lo_full.c_str(), &lo_sb) == 0;

    if (in_upper) {
      up = up_full;
      lo = lo_full;
      cur += "/" + comp;
      // If this directory in upper is opaque, lower is hidden from
      // here on down.
      if (S_ISDIR(up_sb.st_mode) && is_opaque_dir(up_full))
        opaque_above = true;
      // If we're at the leaf, record upper as the answer.
      if (!slash) {
        r.where = Where::kUpper;
        r.path = up_full;
        r.sb = up_sb;
        return r;
      }
      p = slash + 1;
      continue;
    }

    if (in_lower) {
      // Lower-only entry.  We can keep walking if this is a directory.
      lo = lo_full;
      up = up_full;  // for whiteout/opaque checks below
      cur += "/" + comp;
      if (!slash) {
        r.where = Where::kLower;
        r.path = lo_full;
        r.sb = lo_sb;
        return r;
      }
      // For intermediate components, we walk into lower's tree but
      // future components in upper still need to be checked.  Update
      // the upper "where we'd be" path so whiteout checks work.
      p = slash + 1;
      continue;
    }

    // Component missing from both layers — entry does not exist.
    r.where = Where::kNone;
    return r;
  }

  // Empty path after the leading slash — same as root.
  r.where = Where::kUpper;
  r.path = o->upper;
  ::lstat(o->upper.c_str(), &r.sb);
  return r;
}

// ----------------------------------------------------------------------
// Whiteout / opaque helpers (write side)
// ----------------------------------------------------------------------

// Create the parent directories of upper/<rel> in the upper layer,
// copying mode from the corresponding lower directory if it exists.
// This is needed any time we are about to write into a path whose
// parent directory has not yet been materialised in upper.
int
ensure_upper_parents(Overlay *o, const char *fuse_path)
{
  // Walk the components, creating each missing intermediate dir in
  // upper.  We do not copy ownership — the upper tree is owned by
  // the user running the overlay; uid/gid are tracked via stat.
  std::string up = o->upper;
  std::string lo = o->lower;
  const char *p = fuse_path + 1;  // skip leading '/'
  while (true) {
    const char *slash = std::strchr(p, '/');
    if (!slash)
      break;  // last component is the leaf itself; not our concern
    std::string comp(p, slash - p);
    up += "/";
    up += comp;
    lo += "/";
    lo += comp;
    struct stat sb;
    if (::lstat(up.c_str(), &sb) == 0) {
      if (!S_ISDIR(sb.st_mode))
        return -ENOTDIR;
    }
    else {
      mode_t mode = 0755;
      struct stat lsb;
      if (::lstat(lo.c_str(), &lsb) == 0 && S_ISDIR(lsb.st_mode))
        mode = lsb.st_mode & 07777;
      if (::mkdir(up.c_str(), mode) != 0)
        return -errno;
    }
    p = slash + 1;
  }
  return 0;
}

// Remove either form of whiteout for fuse_path's leaf in the upper
// layer.  Used when re-creating an entry that was previously deleted
// (mkdir/create on a path that has a whiteout).  Failures other than
// ENOENT are returned to the caller.
int
clear_whiteout(Overlay *o, const char *fuse_path)
{
  auto [parent, name] = split(fuse_path);
  std::string up_parent = join(o->upper, parent.c_str());
  std::string up_full = up_parent + "/" + name;
  struct stat sb;
  if (::lstat(up_full.c_str(), &sb) == 0 && is_whiteout_dev(sb)) {
    if (::unlink(up_full.c_str()) != 0 && errno != ENOENT)
      return -errno;
  }
  std::string sc = up_parent + "/" + std::string(WH_PREFIX) + name;
  if (::lstat(sc.c_str(), &sb) == 0) {
    if (::unlink(sc.c_str()) != 0 && errno != ENOENT)
      return -errno;
  }
  return 0;
}

// Place a whiteout marker at fuse_path's leaf in upper.  Try the
// canonical char-device form first; if mknod denies us we drop a
// sidecar regular file.  ensure_upper_parents must have run first.
int
place_whiteout(Overlay *o, const char *fuse_path)
{
  auto [parent, name] = split(fuse_path);
  std::string up_parent = join(o->upper, parent.c_str());
  std::string up_full = up_parent + "/" + name;

  // If something is already at upper/<leaf>, remove it first — it
  // might be an old copy-up we are now whiting out.
  struct stat sb;
  if (::lstat(up_full.c_str(), &sb) == 0) {
    if (S_ISDIR(sb.st_mode)) {
      if (::rmdir(up_full.c_str()) != 0)
        return -errno;
    }
    else {
      if (::unlink(up_full.c_str()) != 0)
        return -errno;
    }
  }

  if (o->can_mknod_whiteout) {
    if (::mknod(up_full.c_str(), S_IFCHR | 0000, makedev(0, 0)) == 0)
      return 0;
    if (errno != EPERM && errno != EACCES)
      return -errno;
    o->can_mknod_whiteout = false;
    trace("mknod whiteout denied, falling back to sidecar files");
  }

  // Sidecar fallback.  An empty regular file named ".wh.<name>".
  std::string sc = up_parent + "/" + std::string(WH_PREFIX) + name;
  int fd = ::open(sc.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return -errno;
  ::close(fd);
  return 0;
}

// ----------------------------------------------------------------------
// Copy-up
// ----------------------------------------------------------------------

// Copy a regular file from src (in lower) into dst (in upper) via
// a temp name in the same directory followed by rename, so an
// interrupted copy never leaves a partial file visible.  Preserves
// mode and times; chown is best-effort (we cannot grant ownership
// the running uid does not have).
int
copy_regular(const std::string &src, const std::string &dst,
             const struct stat &src_sb)
{
  int sfd = ::open(src.c_str(), O_RDONLY | O_NOFOLLOW);
  if (sfd < 0)
    return -errno;

  // Temp name in the destination directory.  Use a simple
  // process+counter scheme; collisions retry.
  std::string tmp;
  int dfd = -1;
  for (int attempt = 0; attempt < 100; ++attempt) {
    tmp = dst + ".jaim-cu." + std::to_string(::getpid()) + "." +
          std::to_string(attempt);
    dfd = ::open(tmp.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                 src_sb.st_mode & 07777);
    if (dfd >= 0)
      break;
    if (errno != EEXIST) {
      ::close(sfd);
      return -errno;
    }
  }
  if (dfd < 0) {
    ::close(sfd);
    return -EEXIST;
  }

  // Stream the file contents.  64 KiB buffer is a balance between
  // syscall overhead and stack/heap pressure; copy_file_range would
  // be nicer but is not available on macOS.
  constexpr size_t BUF = 64 * 1024;
  std::vector<char> buf(BUF);
  for (;;) {
    ssize_t n = ::read(sfd, buf.data(), BUF);
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      int e = errno;
      ::close(sfd);
      ::close(dfd);
      ::unlink(tmp.c_str());
      return -e;
    }
    char *p = buf.data();
    ssize_t left = n;
    while (left > 0) {
      ssize_t w = ::write(dfd, p, left);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        int e = errno;
        ::close(sfd);
        ::close(dfd);
        ::unlink(tmp.c_str());
        return -e;
      }
      p += w;
      left -= w;
    }
  }
  ::close(sfd);

  // Preserve times.  We do this on the temp file so they survive
  // the rename.  Use futimes when available (macOS provides it).
  struct timeval tv[2];
  tv[0].tv_sec = src_sb.st_atime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = src_sb.st_mtime;
  tv[1].tv_usec = 0;
  ::futimes(dfd, tv);

  // Best-effort chown.  Ignore failures — we will simply own the
  // upper copy as our running uid, which is fine when the user is
  // running their own sandbox.
  ::fchown(dfd, src_sb.st_uid, src_sb.st_gid);

  ::close(dfd);

  if (::rename(tmp.c_str(), dst.c_str()) != 0) {
    int e = errno;
    ::unlink(tmp.c_str());
    return -e;
  }
  return 0;
}

// Copy a symlink from lower to upper.  Symlinks are always small;
// we read the target with readlink and recreate it.
int
copy_symlink(const std::string &src, const std::string &dst,
             const struct stat &src_sb)
{
  std::vector<char> buf(src_sb.st_size + 1);
  ssize_t n = ::readlink(src.c_str(), buf.data(), buf.size() - 1);
  if (n < 0)
    return -errno;
  buf[n] = 0;
  if (::symlink(buf.data(), dst.c_str()) != 0)
    return -errno;
  return 0;
}

// Make sure the file at fuse_path has a writable upper representation.
// If it is already in upper, nothing to do.  If it is in lower and we
// know how to copy it (regular files and symlinks), we materialise
// the parent directories in upper and copy the leaf.  Other types
// (sockets, char/block devices) are not copied — writes against them
// were never going to make sense in an overlay and we surface EPERM.
int
copy_up(Overlay *o, const char *fuse_path)
{
  Resolved r = resolve(o, fuse_path);
  if (r.where == Where::kUpper)
    return 0;
  if (r.where == Where::kWhiteout || r.where == Where::kNone)
    return -ENOENT;

  if (int e = ensure_upper_parents(o, fuse_path); e < 0)
    return e;

  std::string lo_full = join(o->lower, fuse_path);
  std::string up_full = join(o->upper, fuse_path);

  if (S_ISREG(r.sb.st_mode))
    return copy_regular(lo_full, up_full, r.sb);
  if (S_ISLNK(r.sb.st_mode))
    return copy_symlink(lo_full, up_full, r.sb);
  if (S_ISDIR(r.sb.st_mode)) {
    // Materialise the directory in upper.  We do not copy descendants
    // — they remain visible via lower fall-through until they are
    // themselves modified.
    if (::mkdir(up_full.c_str(), r.sb.st_mode & 07777) != 0 && errno != EEXIST)
      return -errno;
    return 0;
  }
  return -EPERM;
}

// ----------------------------------------------------------------------
// FUSE operations.  All take FUSE-style absolute paths and return
// negative errno on failure.
// ----------------------------------------------------------------------

int
op_getattr(const char *path, struct stat *st)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  *st = r.sb;
  return 0;
}

int
op_fgetattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
  if (fi && fi->fh) {
    if (::fstat(static_cast<int>(fi->fh), st) != 0)
      return -errno;
    return 0;
  }
  return op_getattr(path, st);
}

int
op_access(const char *path, int mask)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  if (::access(r.path.c_str(), mask) != 0)
    return -errno;
  return 0;
}

int
op_readlink(const char *path, char *buf, size_t size)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  ssize_t n = ::readlink(r.path.c_str(), buf, size - 1);
  if (n < 0)
    return -errno;
  buf[n] = 0;
  return 0;
}

// Merged readdir: list upper first (skipping whiteout markers and
// the opaque-directory marker), then list lower (skipping anything
// already produced by upper or whited-out by upper or hidden by an
// opaque marker in upper).  Names are uniqued via a simple set so
// that an entry appearing in both layers is reported once and from
// upper.
int
op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
           off_t /*offset*/, struct fuse_file_info * /*fi*/)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  if (!S_ISDIR(r.sb.st_mode))
    return -ENOTDIR;

  filler(buf, ".", nullptr, 0);
  filler(buf, "..", nullptr, 0);

  // We need to know what the upper directory (if any) hides.
  std::string up_dir = join(o->upper, path);
  std::string lo_dir = join(o->lower, path);

  // Track names already emitted, plus names whited out by upper.
  // Linear vectors are fine for typical directory sizes; avoid
  // dragging in unordered_set for the common small case.
  std::vector<std::string> seen;
  std::vector<std::string> whited;
  bool opaque = false;

  struct stat up_sb;
  bool have_upper = ::lstat(up_dir.c_str(), &up_sb) == 0 &&
                    S_ISDIR(up_sb.st_mode);

  if (have_upper) {
    if (is_opaque_dir(up_dir))
      opaque = true;
    DIR *d = ::opendir(up_dir.c_str());
    if (d) {
      while (auto de = ::readdir(d)) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..")
          continue;
        if (nm == OPAQUE_MARKER)
          continue;
        if (nm.size() > WH_PREFIX_LEN &&
            std::strncmp(nm.c_str(), WH_PREFIX, WH_PREFIX_LEN) == 0) {
          // Sidecar whiteout: hides the matching name in lower, do
          // not emit the marker itself.
          whited.push_back(nm.substr(WH_PREFIX_LEN));
          continue;
        }
        // Char-device whiteouts: skip them in the listing and add
        // the name to the whited list so we don't pick up the lower
        // version below.
        struct stat ds;
        std::string full = up_dir + "/" + nm;
        if (::lstat(full.c_str(), &ds) == 0 && is_whiteout_dev(ds)) {
          whited.push_back(nm);
          continue;
        }
        filler(buf, nm.c_str(), nullptr, 0);
        seen.push_back(std::move(nm));
      }
      ::closedir(d);
    }
  }

  if (!opaque) {
    DIR *d = ::opendir(lo_dir.c_str());
    if (d) {
      while (auto de = ::readdir(d)) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..")
          continue;
        bool skip = false;
        for (const auto &w : whited)
          if (w == nm) {
            skip = true;
            break;
          }
        if (skip)
          continue;
        for (const auto &s : seen)
          if (s == nm) {
            skip = true;
            break;
          }
        if (skip)
          continue;
        filler(buf, nm.c_str(), nullptr, 0);
      }
      ::closedir(d);
    }
  }
  return 0;
}

int
op_mkdir(const char *path, mode_t mode)
{
  Overlay *o = state();
  if (int e = ensure_upper_parents(o, path); e < 0)
    return e;
  if (int e = clear_whiteout(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::mkdir(up.c_str(), mode & 07777) != 0)
    return -errno;
  // If the lower layer also has a directory at this path, we need to
  // mark our newly created upper dir as opaque so the lower contents
  // don't bleed through.  This matches the behaviour of mkdir on a
  // path that was previously rmdir'd.
  std::string lo = join(o->lower, path);
  struct stat sb;
  if (::lstat(lo.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
    std::string m = up + "/" + OPAQUE_MARKER;
    int fd = ::open(m.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
      ::close(fd);
  }
  return 0;
}

int
op_unlink(const char *path)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  if (S_ISDIR(r.sb.st_mode))
    return -EISDIR;

  // If the file exists in upper, remove it.
  std::string up = join(o->upper, path);
  struct stat sb;
  if (::lstat(up.c_str(), &sb) == 0) {
    if (::unlink(up.c_str()) != 0)
      return -errno;
  }
  // If the file exists (or existed) in lower, place a whiteout to
  // hide it.  We don't check r.where strictly here — if the entry
  // resolved to upper but a same-named lower entry exists, we still
  // need a whiteout so the lower copy doesn't reappear after the
  // upper unlink.
  std::string lo = join(o->lower, path);
  if (::lstat(lo.c_str(), &sb) == 0) {
    if (int e = ensure_upper_parents(o, path); e < 0)
      return e;
    if (int e = place_whiteout(o, path); e < 0)
      return e;
  }
  return 0;
}

int
op_rmdir(const char *path)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  if (!S_ISDIR(r.sb.st_mode))
    return -ENOTDIR;

  // Check the merged view is empty.  We rely on op_readdir to give
  // us the merged listing; an empty merged view is the only safe
  // rmdir.  We open the merged dir via fake filler that records
  // whether anything other than . and .. shows up.
  bool nonempty = false;
  auto check_filler = [](void *priv, const char *name,
                         const struct stat *, off_t) -> int {
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
      return 0;
    *static_cast<bool *>(priv) = true;
    return 1;  // tell readdir to stop
  };
  op_readdir(path, &nonempty, check_filler, 0, nullptr);
  if (nonempty)
    return -ENOTEMPTY;

  // Remove from upper if present.
  std::string up = join(o->upper, path);
  struct stat sb;
  if (::lstat(up.c_str(), &sb) == 0) {
    // The upper dir might still contain whiteout markers and the
    // opaque marker — purge those before rmdir, since rmdir(2) will
    // refuse otherwise.
    if (auto d = ::opendir(up.c_str())) {
      while (auto de = ::readdir(d)) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..")
          continue;
        std::string full = up + "/" + nm;
        ::unlink(full.c_str());
      }
      ::closedir(d);
    }
    if (::rmdir(up.c_str()) != 0)
      return -errno;
  }
  // If lower has a directory at this path, hide it with a whiteout.
  std::string lo = join(o->lower, path);
  if (::lstat(lo.c_str(), &sb) == 0) {
    if (int e = ensure_upper_parents(o, path); e < 0)
      return e;
    if (int e = place_whiteout(o, path); e < 0)
      return e;
  }
  return 0;
}

int
op_symlink(const char *target, const char *path)
{
  Overlay *o = state();
  if (int e = ensure_upper_parents(o, path); e < 0)
    return e;
  if (int e = clear_whiteout(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::symlink(target, up.c_str()) != 0)
    return -errno;
  return 0;
}

int
op_rename(const char *from, const char *to)
{
  Overlay *o = state();
  Resolved rf = resolve(o, from);
  if (rf.where == Where::kNone || rf.where == Where::kWhiteout)
    return -ENOENT;

  // Source must be in upper before we can rename it.  copy_up handles
  // the materialisation; for directories it creates an empty upper
  // dir and we later need to mark it opaque so descendants from lower
  // don't leak through.
  if (rf.where == Where::kLower) {
    if (int e = copy_up(o, from); e < 0)
      return e;
  }

  if (int e = ensure_upper_parents(o, to); e < 0)
    return e;
  if (int e = clear_whiteout(o, to); e < 0)
    return e;

  std::string up_from = join(o->upper, from);
  std::string up_to = join(o->upper, to);

  // If the destination already exists in upper, remove it first so
  // rename(2) semantics on macOS line up with what overlayfs would
  // do (overwrite the destination).  rename(2) on macOS will replace
  // a regular target file, but for directories we want to be
  // explicit.
  struct stat sb;
  if (::lstat(up_to.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode))
    ::rmdir(up_to.c_str());

  if (::rename(up_from.c_str(), up_to.c_str()) != 0)
    return -errno;

  // Place a whiteout at the source if a lower entry would otherwise
  // re-emerge there.
  std::string lo_from = join(o->lower, from);
  if (::lstat(lo_from.c_str(), &sb) == 0) {
    if (int e = ensure_upper_parents(o, from); e < 0)
      return e;
    if (int e = place_whiteout(o, from); e < 0)
      return e;
  }
  return 0;
}

int
op_link(const char *from, const char *to)
{
  Overlay *o = state();
  if (int e = copy_up(o, from); e < 0)
    return e;
  if (int e = ensure_upper_parents(o, to); e < 0)
    return e;
  if (int e = clear_whiteout(o, to); e < 0)
    return e;
  std::string up_from = join(o->upper, from);
  std::string up_to = join(o->upper, to);
  if (::link(up_from.c_str(), up_to.c_str()) != 0)
    return -errno;
  return 0;
}

int
op_chmod(const char *path, mode_t mode)
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::chmod(up.c_str(), mode & 07777) != 0)
    return -errno;
  return 0;
}

int
op_chown(const char *path, uid_t uid, gid_t gid)
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::lchown(up.c_str(), uid, gid) != 0)
    return -errno;
  return 0;
}

int
op_truncate(const char *path, off_t size)
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::truncate(up.c_str(), size) != 0)
    return -errno;
  return 0;
}

int
op_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
  if (fi && fi->fh) {
    if (::ftruncate(static_cast<int>(fi->fh), size) != 0)
      return -errno;
    return 0;
  }
  return op_truncate(path, size);
}

int
op_utimens(const char *path, const struct timespec tv[2])
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  // macOS doesn't have utimensat at the syscall level for many
  // years; use utimes which is universally supported.  Sub-second
  // precision is lost — overlay semantics don't depend on it.
  struct timeval ttv[2];
  ttv[0].tv_sec = tv[0].tv_sec;
  ttv[0].tv_usec = tv[0].tv_nsec / 1000;
  ttv[1].tv_sec = tv[1].tv_sec;
  ttv[1].tv_usec = tv[1].tv_nsec / 1000;
  if (::utimes(up.c_str(), ttv) != 0)
    return -errno;
  return 0;
}

int
op_open(const char *path, struct fuse_file_info *fi)
{
  Overlay *o = state();
  int flags = fi->flags;
  bool wants_write = (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) != 0;
  if (wants_write) {
    if (int e = copy_up(o, path); e < 0)
      return e;
    std::string up = join(o->upper, path);
    int fd = ::open(up.c_str(), flags & ~O_NOFOLLOW);
    if (fd < 0)
      return -errno;
    fi->fh = static_cast<uint64_t>(fd);
    return 0;
  }
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  int fd = ::open(r.path.c_str(), flags);
  if (fd < 0)
    return -errno;
  fi->fh = static_cast<uint64_t>(fd);
  return 0;
}

int
op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  Overlay *o = state();
  if (int e = ensure_upper_parents(o, path); e < 0)
    return e;
  if (int e = clear_whiteout(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  int fd = ::open(up.c_str(), fi->flags | O_CREAT, mode & 07777);
  if (fd < 0)
    return -errno;
  fi->fh = static_cast<uint64_t>(fd);
  return 0;
}

int
op_read(const char * /*path*/, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi)
{
  if (!fi || !fi->fh)
    return -EBADF;
  ssize_t n = ::pread(static_cast<int>(fi->fh), buf, size, offset);
  if (n < 0)
    return -errno;
  return static_cast<int>(n);
}

int
op_write(const char * /*path*/, const char *buf, size_t size, off_t offset,
         struct fuse_file_info *fi)
{
  if (!fi || !fi->fh)
    return -EBADF;
  ssize_t n = ::pwrite(static_cast<int>(fi->fh), buf, size, offset);
  if (n < 0)
    return -errno;
  return static_cast<int>(n);
}

int
op_release(const char * /*path*/, struct fuse_file_info *fi)
{
  if (fi && fi->fh)
    ::close(static_cast<int>(fi->fh));
  return 0;
}

int
op_flush(const char * /*path*/, struct fuse_file_info *fi)
{
  if (fi && fi->fh) {
    // dup+close so flush has well-defined semantics with respect to
    // open file descriptions: it pushes data out to disk without
    // closing the caller's fd.
    int dup = ::dup(static_cast<int>(fi->fh));
    if (dup < 0)
      return -errno;
    ::close(dup);
  }
  return 0;
}

int
op_fsync(const char * /*path*/, int datasync, struct fuse_file_info *fi)
{
  if (!fi || !fi->fh)
    return -EBADF;
  int fd = static_cast<int>(fi->fh);
  int rc = datasync ? ::fcntl(fd, F_FULLFSYNC) : ::fsync(fd);
  if (rc < 0)
    return -errno;
  return 0;
}

int
op_statfs(const char *path, struct statvfs *st)
{
  Overlay *o = state();
  // Report stats from upper, where new writes land.  The numbers a
  // process sees from df(1) inside the sandbox should reflect where
  // its own writes consume space.
  Resolved r = resolve(o, path);
  std::string target = (r.where == Where::kNone || r.where == Where::kWhiteout)
                           ? o->upper
                           : r.path;
  if (::statvfs(target.c_str(), st) != 0)
    return -errno;
  return 0;
}

int
op_setxattr(const char *path, const char *name, const char *value,
            size_t size, int flags, uint32_t position)
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::setxattr(up.c_str(), name, value, size, position,
                 flags | XATTR_NOFOLLOW) != 0)
    return -errno;
  return 0;
}

int
op_getxattr(const char *path, const char *name, char *value, size_t size,
            uint32_t position)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  ssize_t n =
      ::getxattr(r.path.c_str(), name, value, size, position, XATTR_NOFOLLOW);
  if (n < 0)
    return -errno;
  return static_cast<int>(n);
}

int
op_listxattr(const char *path, char *list, size_t size)
{
  Overlay *o = state();
  Resolved r = resolve(o, path);
  if (r.where == Where::kNone || r.where == Where::kWhiteout)
    return -ENOENT;
  ssize_t n = ::listxattr(r.path.c_str(), list, size, XATTR_NOFOLLOW);
  if (n < 0)
    return -errno;
  return static_cast<int>(n);
}

int
op_removexattr(const char *path, const char *name)
{
  Overlay *o = state();
  if (int e = copy_up(o, path); e < 0)
    return e;
  std::string up = join(o->upper, path);
  if (::removexattr(up.c_str(), name, XATTR_NOFOLLOW) != 0)
    return -errno;
  return 0;
}

void *
op_init(struct fuse_conn_info *)
{
  return fuse_get_context()->private_data;
}

void
op_destroy(void *)
{
}

// ----------------------------------------------------------------------
// Argument parsing and main
// ----------------------------------------------------------------------

constexpr const char *USAGE =
    "Usage: jaim-overlay --lower=DIR --upper=DIR MOUNTPOINT [FUSE_OPTIONS]\n"
    "\n"
    "Mount a copy-on-write overlay filesystem at MOUNTPOINT.  Reads fall\n"
    "through to --lower for any path not yet modified; writes always go\n"
    "to --upper using overlayfs-compatible whiteout / opaque markers.\n"
    "\n"
    "Both --lower and --upper must be absolute paths to existing\n"
    "directories.  --upper must be writable by the current user.\n"
    "\n"
    "Standard FUSE options (-f to stay in foreground, -d for FUSE debug,\n"
    "-o opt for mount options) are passed through to libfuse.  In\n"
    "addition, -o jdebug enables jaim-overlay's own trace logging.\n"
    "\n"
    "macFUSE 5.2 or newer must be installed.  If FUSE is unavailable\n"
    "the mount fails with a clear error and no mount point is left\n"
    "behind.\n";

// Strip our private --lower=, --upper=, and -o jdebug from argv before
// handing the rest to fuse_main.  Returns 0 on success.
int
extract_args(int &argc, char **argv, std::string &lower, std::string &upper)
{
  std::vector<char *> kept;
  kept.reserve(argc);
  kept.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a.starts_with("--lower=")) {
      lower = a.substr(8);
      continue;
    }
    if (a.starts_with("--upper=")) {
      upper = a.substr(8);
      continue;
    }
    if (a == "--lower" || a == "--upper") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "jaim-overlay: %s requires an argument\n",
                     argv[i]);
        return -1;
      }
      if (a == "--lower")
        lower = argv[++i];
      else
        upper = argv[++i];
      continue;
    }
    if (a == "--help" || a == "-h") {
      std::fputs(USAGE, stdout);
      std::exit(0);
    }
    if (a == "-o" && i + 1 < argc &&
        std::string_view(argv[i + 1]) == "jdebug") {
      g_debug = true;
      ++i;
      continue;
    }
    kept.push_back(argv[i]);
  }
  for (size_t j = 0; j < kept.size(); ++j)
    argv[j] = kept[j];
  argc = static_cast<int>(kept.size());
  return 0;
}

// Resolve a path to absolute + canonical form.  realpath is fine here
// because both layers must already exist by the time we mount.
bool
canonicalise(std::string &p)
{
  char buf[PATH_MAX];
  if (!::realpath(p.c_str(), buf))
    return false;
  p = buf;
  return true;
}

}  // namespace

int
main(int argc, char *argv[])
{
  std::string lower, upper;
  if (extract_args(argc, argv, lower, upper) != 0)
    return 2;

  if (lower.empty() || upper.empty()) {
    std::fputs(USAGE, stderr);
    std::fprintf(stderr,
                 "\njaim-overlay: --lower and --upper are both required\n");
    return 2;
  }
  if (!canonicalise(lower)) {
    std::fprintf(stderr, "jaim-overlay: --lower %s: %s\n", lower.c_str(),
                 std::strerror(errno));
    return 2;
  }
  if (!canonicalise(upper)) {
    std::fprintf(stderr, "jaim-overlay: --upper %s: %s\n", upper.c_str(),
                 std::strerror(errno));
    return 2;
  }
  struct stat sb;
  if (::stat(lower.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
    std::fprintf(stderr, "jaim-overlay: --lower %s is not a directory\n",
                 lower.c_str());
    return 2;
  }
  if (::stat(upper.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
    std::fprintf(stderr, "jaim-overlay: --upper %s is not a directory\n",
                 upper.c_str());
    return 2;
  }
  if (::access(upper.c_str(), W_OK) != 0) {
    std::fprintf(stderr, "jaim-overlay: --upper %s is not writable: %s\n",
                 upper.c_str(), std::strerror(errno));
    return 2;
  }
  // No nesting:  if upper is a subdir of lower or vice versa we'd
  // recurse on every operation.  This is a quick, conservative
  // check using string prefix on canonical paths.
  if (lower == upper ||
      (lower.size() > upper.size() &&
       lower.starts_with(upper) && lower[upper.size()] == '/') ||
      (upper.size() > lower.size() &&
       upper.starts_with(lower) && upper[lower.size()] == '/')) {
    std::fprintf(stderr,
                 "jaim-overlay: --lower and --upper must not be nested\n");
    return 2;
  }

  Overlay overlay;
  overlay.lower = std::move(lower);
  overlay.upper = std::move(upper);

  static struct fuse_operations ops {};
  ops.getattr = op_getattr;
  ops.fgetattr = op_fgetattr;
  ops.access = op_access;
  ops.readlink = op_readlink;
  ops.readdir = op_readdir;
  ops.mkdir = op_mkdir;
  ops.symlink = op_symlink;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.rename = op_rename;
  ops.link = op_link;
  ops.chmod = op_chmod;
  ops.chown = op_chown;
  ops.truncate = op_truncate;
  ops.ftruncate = op_ftruncate;
  ops.utimens = op_utimens;
  ops.open = op_open;
  ops.create = op_create;
  ops.read = op_read;
  ops.write = op_write;
  ops.release = op_release;
  ops.flush = op_flush;
  ops.fsync = op_fsync;
  ops.statfs = op_statfs;
  ops.setxattr = op_setxattr;
  ops.getxattr = op_getxattr;
  ops.listxattr = op_listxattr;
  ops.removexattr = op_removexattr;
  ops.init = op_init;
  ops.destroy = op_destroy;
  // Single-threaded for now: overlay state has no synchronisation
  // around the can_mknod_whiteout flag, and casual-mode workloads
  // are bounded by a single sandboxed shell.  Future work: -o
  // multithreaded with locking.
  ops.flag_nullpath_ok = 1;

  int rc = fuse_main(argc, argv, &ops, &overlay);
  if (rc != 0) {
    std::fprintf(stderr,
                 "jaim-overlay: fuse_main exited %d (is macFUSE 5.2+ "
                 "installed?)\n",
                 rc);
  }
  return rc;
}
