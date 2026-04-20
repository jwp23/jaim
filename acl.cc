// SPDX-License-Identifier: GPL-3.0-or-later
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

#include "acl.h"

#include "defer.h"
#include "err.h"

#include <sys/acl.h>
#include <sys/stat.h>

#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <membership.h>
#include <unistd.h>
#include <uuid/uuid.h>

namespace jaim_acl {

namespace {

// acl_free returns int; RaiiHelper only takes a void-returning
// destroyer.  Wrap each type separately so the Destroy signature and
// the stored T line up — a single void* destroyer would work via
// implicit conversion but obscures the ownership at the declaration.
inline void free_acl(acl_t a) { acl_free(a); }
inline void free_qual(void *q) { acl_free(q); }

using AclRef = RaiiHelper<free_acl, acl_t>;
using QualRef = RaiiHelper<free_qual, void *>;

// macOS stores the qualifier of an extended-ACL entry as a uuid_t
// (16-byte kauth guid), not the uid or gid directly.  Conversions go
// through membership.h.  Keep the conversion at the boundaries so
// the rest of the module can work in uid_t.
void
uid_to_uuid(uid_t uid, uuid_t out)
{
  if (mbr_uid_to_uuid(uid, out))
    syserr("mbr_uid_to_uuid({})", uid);
}

// Permission bits the caller composes; here we translate them into
// the kauth ACL_* permissions.  The macOS model is finer-grained
// than POSIX rwx (separate bits for list vs read_data, for example),
// but ACL_READ_DATA doubles as list_directory on a dir, ACL_WRITE_DATA
// as add_file, ACL_APPEND_DATA as add_subdirectory, and ACL_EXECUTE
// as search — so granting the "full" bundle per Perm flag does what
// a user expects for both files and directories.
void
add_perms(acl_permset_t perms, PermMask mask, bool is_dir)
{
  if (mask & kRead) {
    acl_add_perm(perms, ACL_READ_DATA);
    acl_add_perm(perms, ACL_READ_ATTRIBUTES);
    acl_add_perm(perms, ACL_READ_EXTATTRIBUTES);
    acl_add_perm(perms, ACL_READ_SECURITY);
  }
  if (mask & kWrite) {
    acl_add_perm(perms, ACL_WRITE_DATA);
    acl_add_perm(perms, ACL_APPEND_DATA);
    acl_add_perm(perms, ACL_WRITE_ATTRIBUTES);
    acl_add_perm(perms, ACL_WRITE_EXTATTRIBUTES);
    acl_add_perm(perms, ACL_DELETE);
    if (is_dir)
      acl_add_perm(perms, ACL_DELETE_CHILD);
  }
  if (mask & kExecute)
    acl_add_perm(perms, ACL_EXECUTE);
}

bool
path_is_dir(const path &p)
{
  struct stat sb;
  if (lstat(p.c_str(), &sb))
    syserr("lstat({})", p.string());
  return S_ISDIR(sb.st_mode);
}

bool
is_symlink(const struct dirent *de, const path &full)
{
  if (de->d_type == DT_LNK)
    return true;
  if (de->d_type != DT_UNKNOWN)
    return false;
  // Filesystems that don't populate d_type (rare on APFS, but
  // readdir is spec'd to allow DT_UNKNOWN).  A fresh lstat is cheap.
  struct stat sb;
  if (lstat(full.c_str(), &sb))
    return true; // unreachable entries: treat as symlink-like, skip
  return S_ISLNK(sb.st_mode);
}

// Core: fetch the extended ACL (or init an empty one), append an
// allow entry for uid with the right perms and inherit flags, write
// it back.  Callers that want an existing uid-match replaced rather
// than duplicated should remove_user_entries first.
void
do_add_allow_user(const path &p, uid_t uid, PermMask perms, bool is_dir)
{
  // acl_get_file returns NULL with errno=ENOENT when an inode has no
  // extended ACL yet (as distinct from "path does not exist" — same
  // errno, but the parent lstat in the caller already ruled that
  // out).  Some versions of Darwin also leave errno unchanged (==0
  // here if the caller hasn't just failed).  In either case fall
  // back to acl_init.
  AclRef acl = acl_get_file(p.c_str(), ACL_TYPE_EXTENDED);
  if (!acl) {
    if (errno != ENOENT && errno != 0)
      syserr("acl_get_file({})", p.string());
    acl = acl_init(1);
    if (!acl)
      syserr("acl_init");
  }

  acl_entry_t entry;
  if (acl_create_entry(&*acl, &entry))
    syserr("acl_create_entry({})", p.string());

  if (acl_set_tag_type(entry, ACL_EXTENDED_ALLOW))
    syserr("acl_set_tag_type({})", p.string());

  uuid_t uu;
  uid_to_uuid(uid, uu);
  if (acl_set_qualifier(entry, uu))
    syserr("acl_set_qualifier({})", p.string());

  acl_permset_t permset;
  if (acl_get_permset(entry, &permset))
    syserr("acl_get_permset({})", p.string());
  acl_clear_perms(permset);
  add_perms(permset, perms, is_dir);
  if (acl_set_permset(entry, permset))
    syserr("acl_set_permset({})", p.string());

  // Inherit flags on directories so new children pick the grant up
  // without us having to sweep on every sandbox launch.  On regular
  // files the inherit flags are meaningless, so skip them.
  if (is_dir) {
    acl_flagset_t flags;
    if (acl_get_flagset_np(entry, &flags))
      syserr("acl_get_flagset_np({})", p.string());
    acl_clear_flags_np(flags);
    acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT);
    acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT);
    if (acl_set_flagset_np(entry, flags))
      syserr("acl_set_flagset_np({})", p.string());
  }

  if (acl_set_file(p.c_str(), ACL_TYPE_EXTENDED, *acl))
    syserr("acl_set_file({})", p.string());
}

// Walk the ACL, delete every entry whose qualifier-uuid matches
// target_uu, write back if any was removed.  acl_delete_entry
// invalidates the walk cursor, so restart from ACL_FIRST_ENTRY after
// each deletion.  The ACL has at most ACL_MAX_ENTRIES (128 in
// sys/acl.h), so the worst-case quadratic is a wash.
void
do_remove_user_entries(const path &p, const uuid_t target_uu)
{
  AclRef acl = acl_get_file(p.c_str(), ACL_TYPE_EXTENDED);
  if (!acl) {
    if (errno == ENOENT || errno == 0)
      return;
    syserr("acl_get_file({})", p.string());
  }

  bool changed = false;
  for (;;) {
    acl_entry_t entry;
    int r = acl_get_entry(*acl, ACL_FIRST_ENTRY, &entry);
    bool found = false;
    while (r == 0) {
      QualRef q = acl_get_qualifier(entry);
      if (q && uuid_compare(*static_cast<uuid_t *>(*q), target_uu) == 0) {
        if (acl_delete_entry(*acl, entry))
          syserr("acl_delete_entry({})", p.string());
        changed = true;
        found = true;
        break;
      }
      r = acl_get_entry(*acl, ACL_NEXT_ENTRY, &entry);
    }
    if (!found)
      break;
  }

  if (!changed)
    return;

  if (acl_set_file(p.c_str(), ACL_TYPE_EXTENDED, *acl))
    syserr("acl_set_file({})", p.string());
}

// Walk `p` depth-first, calling `fn(child_path, is_dir)` on every
// regular file and directory reachable below it (including `p`
// itself).  Symlinks are skipped so the ACL application stays inside
// the granted subtree — following symlinks would let a .ssh/symlink
// inside a granted dir silently widen the grant.
template<typename F>
void
walk_tree(const path &p, F &&fn)
{
  struct stat sb;
  if (lstat(p.c_str(), &sb)) {
    warn("lstat({}): {}", p.string(), std::strerror(errno));
    return;
  }
  if (S_ISLNK(sb.st_mode))
    return;
  try {
    fn(p, S_ISDIR(sb.st_mode));
  } catch (const std::exception &e) {
    warn("{}", e.what());
  }

  if (!S_ISDIR(sb.st_mode))
    return;

  DIR *d = opendir(p.c_str());
  if (!d) {
    warn("opendir({}): {}", p.string(), std::strerror(errno));
    return;
  }
  Defer _close([d] { closedir(d); });

  while (auto de = readdir(d)) {
    const char *name = de->d_name;
    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
      continue;
    path child = p / name;
    if (is_symlink(de, child))
      continue;
    walk_tree(child, fn);
  }
}

} // namespace

void
add_allow_user(const path &p, uid_t uid, PermMask perms)
{
  do_add_allow_user(p, uid, perms, path_is_dir(p));
}

void
apply_recursive(const path &p, uid_t uid, PermMask perms)
{
  walk_tree(p, [uid, perms](const path &cp, bool is_dir) {
    do_add_allow_user(cp, uid, perms, is_dir);
  });
}

void
remove_user_entries(const path &p, uid_t uid)
{
  uuid_t target;
  uid_to_uuid(uid, target);
  do_remove_user_entries(p, target);
}

void
remove_recursive(const path &p, uid_t uid)
{
  uuid_t target;
  uid_to_uuid(uid, target);
  walk_tree(p, [&target](const path &cp, bool /*is_dir*/) {
    do_remove_user_entries(cp, target);
  });
}

} // namespace jaim_acl
