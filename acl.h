// -*-C++-*-
/*
 * jaim - Sandboxing tool for AI CLI access
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * Extended-ACL helpers for strict-mode UID separation (ja-ekp).  When
 * jaim's strict mode forks and setuids the sandboxed child to the
 * _jaim system user (ja-txx), that user has no POSIX access to the
 * directories and files the invoking user granted via --dir / --file
 * / CWD.  Rather than requiring the user to run everything in a
 * group-shared tree or chmod paths world-accessible, we add a per-
 * invocation macOS extended-ACL entry granting _jaim the needed
 * access, then strip it in the parent's atexit handler after the
 * sandbox has finished.  macOS uses a Kauth-backed ACL model
 * accessible through <sys/acl.h>; the API here is a thin, typed
 * wrapper over acl_get_file / acl_create_entry / acl_set_file that
 * hides the boilerplate (RAII, tag/qualifier/permset plumbing,
 * inherit flags) from the callsite.
 */

#pragma once

#include <filesystem>
#include <sys/types.h>

namespace jaim_acl {

using std::filesystem::path;

// Permission bits the caller composes into a PermMask.  Mapped
// internally to the macOS ACL_* perm bits in sys/acl.h, which are
// finer-grained than POSIX rwx but have obvious rwx groupings for
// the sandbox-grant use case.  kRead covers list-dir + read-data +
// read-attrs; kWrite covers write-data + append + add-file +
// add-subdir + delete-child + delete + write-attrs; kExecute covers
// the search/execute bit that macOS overloads for both file exec and
// directory traversal.
enum Perm : unsigned {
  kRead = 1u << 0,
  kWrite = 1u << 1,
  kExecute = 1u << 2,
};
using PermMask = unsigned;
inline constexpr PermMask kReadExec = kRead | kExecute;
inline constexpr PermMask kReadWriteExec = kRead | kWrite | kExecute;

// Add an allow ACL entry for `uid` to the file or directory at `p`.
// If `p` is a directory, the entry includes file_inherit +
// directory_inherit so descendants created later pick up the grant.
// Idempotent in the sense that repeated calls with identical perms
// add a duplicate entry — the kernel tolerates duplicates, and
// remove_user_entries strips every entry for the uid, so the
// add/remove pair composes correctly.  Does NOT recursively modify
// existing children — callers that need that use apply_recursive.
// Throws std::system_error on failure.
void add_allow_user(const path &p, uid_t uid, PermMask perms);

// add_allow_user on `p` and every regular file / directory reachable
// below it.  Symlinks are ignored (ACLs on the link target would
// escape the granted subtree, which is exactly what the sandbox is
// trying to prevent).  Failures on individual descendants are
// reported via warn() but do not abort the sweep: a partial grant
// is recoverable (remove_recursive is similarly best-effort) while
// an aborting partial grant would leave the filesystem in a half-
// modified state that is harder to clean up.
void apply_recursive(const path &p, uid_t uid, PermMask perms);

// Remove every allow/deny ACL entry whose qualifier matches `uid`
// from the file or directory at `p`.  No-op if no such entry exists.
// Used to undo add_allow_user().  Throws std::system_error on
// failure.
void remove_user_entries(const path &p, uid_t uid);

// remove_user_entries on `p` and every regular file / directory
// reachable below it.  Mirrors apply_recursive: skips symlinks,
// continues past per-entry failures with a warn() so a single
// permission error does not leave the rest of the tree carrying
// stale _jaim ACL entries.
void remove_recursive(const path &p, uid_t uid);

} // namespace jaim_acl
