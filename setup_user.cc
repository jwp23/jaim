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
 *
 * setup_user.cc — create and remove the _jaim system user via dscl,
 * and install jaim itself as setuid root.  Backs the --setup-user,
 * --remove-user, and --setup-setuid one-shot flags in jaim.cc.  The
 * _jaim user is a prerequisite for strict-mode UID separation
 * (ja-txx):  dropping to an unprivileged account before sandbox_init
 * puts the sandboxed child under macOS TCC as a principal with no
 * granted permissions (no camera, mic, contacts, calendar, etc.),
 * which the sandbox profile alone cannot enforce.  --setup-setuid
 * (ja-fe2) is the second half of the same goal:  with jaim installed
 * setuid root, users can invoke `jaim -m strict` without sudo, and
 * do_main() drops privileges immediately so the unprivileged phase
 * of the program cannot exploit the elevated euid.
 *
 * Modified 2026 by Joseph Presley: add setup_setuid() and
 *   running_executable_path() helpers backing --setup-setuid (ja-fe2).
 */

#include "cred.h"
#include "err.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <print>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <mach-o/dyld.h>

extern "C" char **environ;

namespace {

constexpr const char *kDscl = "/usr/bin/dscl";
constexpr const char *kHomeDir = "/var/empty";
constexpr const char *kShell = "/usr/bin/false";
constexpr const char *kRealName = "jaim Sandbox User";
constexpr const char *kGroupRealName = "jaim Sandbox Group";

// UID/GID scan range.  macOS reserves < 500 for system accounts; we
// start at 300 because the < 300 range is densely populated by Apple
// daemons and the 300-500 band has plenty of gaps on a typical host.
// The scan picks the first unused value, so an exact UID is not
// guaranteed across machines — which is fine: jaim looks the user up
// by name (kJaimSystemUser) and never embeds a numeric UID anywhere.
constexpr ugid_t kScanStart = 300;
constexpr ugid_t kScanEnd = 500;

// Fork/exec argv and wait for it to exit.  Returns the child's exit
// status (0 on success, nonzero on failure).  Signal-killed or stuck
// children throw.  No stdin/stdout plumbing — dscl writes its errors
// to stderr, which is inherited, and we surface the exit code.
int
run_cmd(std::vector<const char *> argv)
{
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid == -1)
    syserr("fork");
  if (pid == 0) {
    execve(argv[0], const_cast<char *const *>(argv.data()), environ);
    // execve returned — exec failed.  Report via exit code 127 (the
    // convention for "command not found / exec failed") so the parent
    // can distinguish exec failure from a non-zero exit by the
    // target.
    perror(argv[0]);
    _exit(127);
  }

  int status;
  while (waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR)
      syserr("waitpid({})", argv[0]);
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    err("{}: killed by signal {}", argv[0], WTERMSIG(status));
  err("{}: unexpected wait status {}", argv[0], status);
}

// Pick the lowest unused UID in [kScanStart, kScanEnd).  Looked up via
// PwEnt::get_id, which goes through getpwuid_r and therefore sees
// everything OpenDirectory knows about — including ephemeral network
// accounts, not just /etc/passwd.  A simultaneously-racing process
// claiming the same UID would still get caught by dscl's own uniqueness
// check when we call -create; the scan just keeps the common case from
// colliding.
ugid_t
find_unused_uid()
{
  for (ugid_t u = kScanStart; u < kScanEnd; ++u)
    if (!PwEnt::get_id(u))
      return u;
  err("could not find an unused UID in [{}, {})", kScanStart, kScanEnd);
}

ugid_t
find_unused_gid()
{
  for (ugid_t g = kScanStart; g < kScanEnd; ++g)
    if (!GrEnt::get_id(g))
      return g;
  err("could not find an unused GID in [{}, {})", kScanStart, kScanEnd);
}

// dscl . -create <record> <key> <value>.  Thin wrapper so each
// attribute set reads as one line at the callsite.
void
dscl_set(const char *record, const char *key, const std::string &value)
{
  int r = run_cmd({kDscl, ".", "-create", record, key, value.c_str()});
  if (r != 0)
    err("dscl . -create {} {}: exit {}", record, key, r);
}
void
dscl_set(const char *record, const char *key, const char *value)
{
  dscl_set(record, key, std::string(value));
}

void
dscl_create_record(const char *record)
{
  int r = run_cmd({kDscl, ".", "-create", record});
  if (r != 0)
    err("dscl . -create {}: exit {}", record, r);
}

void
dscl_delete_record(const char *record)
{
  int r = run_cmd({kDscl, ".", "-delete", record});
  if (r != 0)
    err("dscl . -delete {}: exit {}", record, r);
}

// Resolve the path of the currently-running executable.  argv[0] is
// not enough because it may be relative ("./jaim"), PATH-resolved
// ("jaim"), or a symlink — and chmod/chown on a symlink would mutate
// the link's target permissions on macOS but not the link itself,
// which is fine but awkward when we want to report what we actually
// touched.  _NSGetExecutablePath returns the path used to load the
// binary; realpath canonicalizes through any symlinks.  Two-call
// idiom: the first call with a null buffer reports the required
// size in bufsize.
std::string
running_executable_path()
{
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0)
    err("_NSGetExecutablePath: failed to retrieve executable path");
  char resolved[PATH_MAX];
  if (!realpath(buf.data(), resolved))
    syserr("realpath({})", buf.data());
  return std::string(resolved);
}

void
require_root(const char *flag)
{
  if (geteuid() != 0)
    err("{} requires root; rerun with sudo", flag);
}

} // namespace

void
setup_jaim_user()
{
  require_root("--setup-user");

  // Idempotent short-circuit: if the account is already present we
  // report and bail.  We deliberately do not try to repair mismatched
  // attributes (e.g. a prior _jaim with a different shell) — the user
  // can --remove-user first and rerun --setup-user if they want a
  // fresh setup.  Warn on surprising attributes so a hand-edited
  // record does not silently diverge from jaim's expectations.
  if (PwEnt pw = PwEnt::get_nam(kJaimSystemUser)) {
    std::println("user {} already exists (uid={}, gid={})",
                 kJaimSystemUser, pw->pw_uid, pw->pw_gid);
    std::string home = pw->pw_dir ? pw->pw_dir : "";
    std::string shell = pw->pw_shell ? pw->pw_shell : "";
    if (home != kHomeDir)
      warn("existing {}: NFSHomeDirectory={} (expected {})",
           kJaimSystemUser, home, kHomeDir);
    if (shell != kShell)
      warn("existing {}: UserShell={} (expected {})",
           kJaimSystemUser, shell, kShell);
    return;
  }

  const std::string user_record = std::string("/Users/") + kJaimSystemUser;
  const std::string group_record = std::string("/Groups/") + kJaimSystemUser;

  // Group first: the user's PrimaryGroupID must reference an existing
  // gid (or dscl errors out), and a user without a valid primary
  // group can't be looked up by login name anyway.
  ugid_t gid = find_unused_gid();
  dscl_create_record(group_record.c_str());
  try {
    dscl_set(group_record.c_str(), "PrimaryGroupID", std::to_string(gid));
    dscl_set(group_record.c_str(), "RealName", kGroupRealName);
    dscl_set(group_record.c_str(), "Password", "*");
  } catch (...) {
    // Creation partially succeeded; clean up so a rerun finds a
    // coherent state rather than a half-populated group record.
    try { dscl_delete_record(group_record.c_str()); } catch (...) {}
    throw;
  }

  // Verify the group is resolvable before we reference it in the user
  // record.  OpenDirectory usually exposes local records immediately,
  // but this is cheap insurance.
  if (!GrEnt::get_nam(kJaimSystemUser))
    err("{} group was not created successfully", kJaimSystemUser);

  ugid_t uid = find_unused_uid();
  dscl_create_record(user_record.c_str());
  try {
    dscl_set(user_record.c_str(), "UniqueID", std::to_string(uid));
    dscl_set(user_record.c_str(), "PrimaryGroupID", std::to_string(gid));
    dscl_set(user_record.c_str(), "NFSHomeDirectory", kHomeDir);
    dscl_set(user_record.c_str(), "UserShell", kShell);
    dscl_set(user_record.c_str(), "RealName", kRealName);
    dscl_set(user_record.c_str(), "Password", "*");
    // IsHidden=1 keeps the account out of the macOS login screen's
    // user picker; system accounts with UID < 500 are already hidden
    // by loginwindow, but this is the documented belt-and-braces
    // approach and costs nothing.
    dscl_set(user_record.c_str(), "IsHidden", "1");
  } catch (...) {
    try { dscl_delete_record(user_record.c_str()); } catch (...) {}
    try { dscl_delete_record(group_record.c_str()); } catch (...) {}
    throw;
  }

  if (!PwEnt::get_nam(kJaimSystemUser))
    err("{} user was not created successfully", kJaimSystemUser);

  std::println("created {} (uid={}, gid={}, home={}, shell={})",
               kJaimSystemUser, uid, gid, kHomeDir, kShell);
}

void
remove_jaim_user()
{
  require_root("--remove-user");

  const std::string user_record = std::string("/Users/") + kJaimSystemUser;
  const std::string group_record = std::string("/Groups/") + kJaimSystemUser;

  bool removed = false;

  if (PwEnt::get_nam(kJaimSystemUser)) {
    dscl_delete_record(user_record.c_str());
    std::println("removed user {}", kJaimSystemUser);
    removed = true;
  }

  if (GrEnt::get_nam(kJaimSystemUser)) {
    dscl_delete_record(group_record.c_str());
    std::println("removed group {}", kJaimSystemUser);
    removed = true;
  }

  if (!removed)
    std::println("user and group {} not present; nothing to remove",
                 kJaimSystemUser);
}

void
setup_setuid()
{
  require_root("--setup-setuid");

  std::string p = running_executable_path();

  // chown to root:wheel.  macOS's setuid binaries conventionally
  // belong to root:wheel (gid 0); chown'ing both fields is a no-op
  // when the binary is already root-owned, and corrects ownership
  // when the user installed jaim as themselves.  Without this step,
  // chmod(... | S_ISUID) can succeed but produce a binary whose
  // setuid bit elevates to the wrong user.
  if (chown(p.c_str(), 0, 0))
    syserr("chown(root:wheel) {}", p);

  // Mode 4555: setuid + r-x for owner/group/other; deliberately no
  // write bits.  No-write makes the binary tamper-resistant — a
  // post-install attacker who lands as root could of course just
  // chmod it back, but this closes the easier path of overwriting
  // the binary in place via a non-root process that happens to have
  // ended up with write access from a botched install.  setuid bit
  // is what actually enables the no-sudo strict-mode use case.
  mode_t mode = S_ISUID | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP
              | S_IROTH | S_IXOTH;
  if (chmod(p.c_str(), mode))
    syserr("chmod(4555) {}", p);

  std::println("installed {} as setuid root (mode 4555)", p);
  std::println("you can now run `{} -m strict` without sudo",
               prog.filename().string());
}
