// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Joseph Presley
//
// Modified 2026 by Joseph Presley: rewrite from Linux POSIX-ACL xattr
//   round-trip tests to macOS Kauth-ACL coverage for the new acl.h
//   public API (add / apply_recursive / remove / remove_recursive).
//   The Linux tests did not translate: no xattr serialize form, no
//   16-entry ACL class/mask/default structure on macOS (ja-ekp).
//
// Unit test for the macOS extended-ACL helper module (acl.cc / acl.h,
// added in ja-ekp).  The file kept its upstream-jai name
// (fs_acl_test.cc) when the jaim port moved ACL manipulation out of
// fs.h into its own module; the Linux POSIX-ACL tests it used to
// hold do not translate to the macOS Kauth ACL model (no xattr
// serialize/deserialize round-trip, no 16-entry ACL class/mask/
// default structure).  The exercises here cover the public API that
// callers actually use: add, recurse, remove, and round-trip.  The
// tests only need the invoking user's own uid, which keeps the
// coverage useful under `make check` without requiring root or a
// real _jaim system account.

#include "../acl.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <membership.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

// err.h references a `prog` global that lives in jaim.cc; the test
// binary links only acl.o, so we have to define it here to satisfy
// the warn() path in the walk helpers.
std::filesystem::path prog = "fs_acl_test";

namespace {

using std::filesystem::path;
using namespace jaim_acl;

struct TempDir {
  path p;
  TempDir()
  {
    char tmpl[] = "/tmp/jaim-acl-test.XXXXXX";
    char *r = mkdtemp(tmpl);
    assert(r);
    p = r;
  }
  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
  }
};

bool
acl_unsupported(const std::system_error &e)
{
  int c = e.code().value();
  return c == ENOTSUP || c == EOPNOTSUPP;
}

// Return the number of extended-ACL entries whose qualifier matches
// `uid`.  0 means "no entry for this user", 1+ means at least one
// grant exists.  Used as the ground-truth oracle for the tests
// below — the public API is opaque otherwise, so we peek at the
// Kauth layer directly.
int
count_entries_for_uid(const path &p, uid_t uid)
{
  uuid_t target;
  if (mbr_uid_to_uuid(uid, target))
    throw std::system_error(errno, std::system_category(), "mbr_uid_to_uuid");

  acl_t acl = acl_get_file(p.c_str(), ACL_TYPE_EXTENDED);
  if (!acl) {
    if (errno == ENOENT || errno == 0)
      return 0;
    throw std::system_error(errno, std::system_category(),
                            "acl_get_file " + p.string());
  }

  int count = 0;
  acl_entry_t entry;
  int r = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
  while (r == 0) {
    void *q = acl_get_qualifier(entry);
    if (q) {
      if (uuid_compare(*static_cast<uuid_t *>(q), target) == 0)
        ++count;
      acl_free(q);
    }
    r = acl_get_entry(acl, ACL_NEXT_ENTRY, &entry);
  }
  acl_free(acl);
  return count;
}

void
write_file(const path &p, const char *content = "")
{
  std::ofstream(p) << content;
}

void
test_add_then_remove_on_file()
{
  TempDir td;
  path f = td.p / "file";
  write_file(f, "hi");

  uid_t u = getuid();
  assert(count_entries_for_uid(f, u) == 0);

  add_allow_user(f, u, kRead);
  assert(count_entries_for_uid(f, u) == 1);

  remove_user_entries(f, u);
  assert(count_entries_for_uid(f, u) == 0);
}

void
test_add_then_remove_on_dir()
{
  TempDir td;
  path d = td.p / "d";
  assert(std::filesystem::create_directory(d));

  uid_t u = getuid();
  assert(count_entries_for_uid(d, u) == 0);

  add_allow_user(d, u, kReadWriteExec);
  assert(count_entries_for_uid(d, u) == 1);

  remove_user_entries(d, u);
  assert(count_entries_for_uid(d, u) == 0);
}

void
test_remove_on_file_with_no_acl_is_noop()
{
  TempDir td;
  path f = td.p / "empty";
  write_file(f);

  // Must not throw or change the ACL state.
  remove_user_entries(f, getuid());
  assert(count_entries_for_uid(f, getuid()) == 0);
}

void
test_apply_recursive_covers_existing_children()
{
  TempDir td;
  path root = td.p / "tree";
  assert(std::filesystem::create_directory(root));
  path sub = root / "sub";
  assert(std::filesystem::create_directory(sub));
  path file_a = root / "a";
  path file_b = sub / "b";
  write_file(file_a, "a");
  write_file(file_b, "b");

  uid_t u = getuid();
  assert(count_entries_for_uid(root, u) == 0);
  assert(count_entries_for_uid(sub, u) == 0);
  assert(count_entries_for_uid(file_a, u) == 0);
  assert(count_entries_for_uid(file_b, u) == 0);

  apply_recursive(root, u, kReadWriteExec);
  assert(count_entries_for_uid(root, u) == 1);
  assert(count_entries_for_uid(sub, u) == 1);
  assert(count_entries_for_uid(file_a, u) == 1);
  assert(count_entries_for_uid(file_b, u) == 1);

  remove_recursive(root, u);
  assert(count_entries_for_uid(root, u) == 0);
  assert(count_entries_for_uid(sub, u) == 0);
  assert(count_entries_for_uid(file_a, u) == 0);
  assert(count_entries_for_uid(file_b, u) == 0);
}

void
test_walk_skips_symlinks()
{
  TempDir td;
  path root = td.p / "root";
  assert(std::filesystem::create_directory(root));
  path real = td.p / "outside";
  write_file(real, "outside");

  // A symlink inside `root` pointing at an outside file.  The walk
  // must leave `outside` untouched — following the link would widen
  // the grant beyond the subtree the caller asked for.
  path link = root / "link";
  if (symlink(real.c_str(), link.c_str()) != 0) {
    std::fprintf(stderr, "symlink: %s — skipping symlink test\n",
                 std::strerror(errno));
    return;
  }

  uid_t u = getuid();
  apply_recursive(root, u, kRead);
  assert(count_entries_for_uid(root, u) == 1);
  assert(count_entries_for_uid(real, u) == 0); // outside file NOT touched

  remove_recursive(root, u);
  assert(count_entries_for_uid(root, u) == 0);
}

void
test_remove_leaves_other_uids_alone()
{
  // We can't create a second UID in a unit test without root, but we
  // can add an entry for our own uid, add one via a different code
  // path that uses a different *uuid* (a synthetic gid→uuid that will
  // not collide with our uid→uuid), and confirm remove_user_entries
  // only drops the targeted one.  Synthesizing another qualifier
  // without membership services is awkward, so instead we verify the
  // simpler property: two successive add_allow_user calls produce two
  // entries (the wrapper intentionally does not dedupe), and one
  // remove_user_entries drops both.  Callers that want dedupe are
  // expected to remove first.
  TempDir td;
  path f = td.p / "dup";
  write_file(f);

  uid_t u = getuid();
  add_allow_user(f, u, kRead);
  add_allow_user(f, u, kWrite);
  assert(count_entries_for_uid(f, u) == 2);

  remove_user_entries(f, u);
  assert(count_entries_for_uid(f, u) == 0);
}

} // namespace

int
main()
{
  try {
    test_add_then_remove_on_file();
    test_add_then_remove_on_dir();
    test_remove_on_file_with_no_acl_is_noop();
    test_apply_recursive_covers_existing_children();
    test_walk_skips_symlinks();
    test_remove_leaves_other_uids_alone();
  } catch (const std::system_error &e) {
    if (acl_unsupported(e)) {
      std::fprintf(stderr, "skipping: filesystem does not support ACLs: %s\n",
                   e.what());
      return 77;
    }
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "PASS: fs_acl_test\n");
  return 0;
}
