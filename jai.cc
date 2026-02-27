#include "jai.h"

#include <cassert>
#include <print>

#include <acl/libacl.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

path prog;

constexpr const char *kRunRoot = "/run/jai";
constexpr const char *kSB = "sandboxed-home";

struct Config {
  std::string user_;
  uid_t uid_ = -1;
  gid_t gid_ = -1;
  path homepath_;

  Fd homefd_;
  Fd home_jai_fd_;
  Fd run_jai_fd_;
  Fd run_home_fd_;

  void init();

  Fd make_home_overlay();
  Fd make_root_dir();
  Fd make_ns();

  [[nodiscard]] Defer asuser();
  int homejai();
  int runjai();
  int runhome();

  struct NsState {
    Fd rootfd;
    Fd pipefds[2];
  };
};

void
Config::init()
{
  char buf[512];
  struct passwd pwbuf, *pw{};

  auto realuid = getuid();

  const char *envuser = getenv("SUDO_USER");
  if (realuid == 0 && envuser) {
    if (getpwnam_r(envuser, &pwbuf, buf, sizeof(buf), &pw))
      err("cannot find password entry for user {}", envuser);
  }
  else if (getpwuid_r(realuid, &pwbuf, buf, sizeof(buf), &pw))
    err("cannot find password entry for uid {}", uid_);

  user_ = pw->pw_name;
  uid_ = pw->pw_uid;
  gid_ = pw->pw_gid;
  homepath_ = pw->pw_dir;

  // Paranoia about ptrace, because we will drop privileges to access
  // the file system as the user.
  prctl(PR_SET_DUMPABLE, 0);

  // Set all user permissions except user ID so we can easily drop
  // privileges in asuser.
  if (realuid == 0 && uid_ != 0) {
    if (initgroups(user_.c_str(), gid_))
      syserr("initgroups");
    if (setgid(gid_))
      syserr("setgid");
  }

  auto cleanup = asuser();
  if (!(homefd_ = open(homepath_.c_str(), O_PATH | O_CLOEXEC)))
    syserr("{}", homepath_.string());
}

Defer
Config::asuser()
{
  if (!uid_ || geteuid())
    // If target is root or already dropped privileges, do nothing
    return {};
  if (seteuid(uid_))
    syserr("seteuid");
  return Defer{[] { seteuid(0); }};
}

int
Config::homejai()
{
  if (!home_jai_fd_) {
    auto restore = asuser();
    home_jai_fd_ = ensure_dir(*homefd_, ".jai", 0700, kFollow);
  }
  return *home_jai_fd_;
}

int
Config::runjai()
{
  if (run_jai_fd_)
    return *run_jai_fd_;

  static const auto lockfile = std::format("{}.lock", kRunRoot);

  Fd lock;
  for (;;) {
    struct stat sb;
    if (Fd fd = open(kRunRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        fd && !fstatat(*fd, ".initialized", &sb, AT_SYMLINK_NOFOLLOW))
      return *(run_jai_fd_ = std::move(fd));
    if (lock)
      break;
    lock = open_lockfile(-1, lockfile);
  }

  // Get rid of any partially set up directories
  recursive_umount(kRunRoot);

  Fd mfd = make_tmpfs("size", "64M", "mode", "0755", "gid", "0");
  xmnt_move(*mfd, *ensure_dir(-1, kRunRoot, 0755, kFollow));

  run_jai_fd_ = ensure_dir(-1, kRunRoot, 0755, kFollow);
  xmnt_propagate(*run_jai_fd_, MS_PRIVATE);
  xopenat(*run_jai_fd_, ".initialized", O_CREAT | O_WRONLY, 0444);
  unlink(lockfile.c_str());
  return *run_jai_fd_;
}

int
Config::runhome()
{
  if (run_home_fd_)
    return *run_home_fd_;

  run_home_fd_ = ensure_dir(runjai(), user_, 0700, kNoFollow);
  RaiiHelper<acl_free, acl_t> acl = acl_get_fd(*run_home_fd_);
  if (!acl)
    syserr("acl_get_fd");
  if (int r = acl_equiv_mode(acl, nullptr); r < 0)
    syserr("acl_equiv_mode");
  else if (r == 0) {
    auto text = std::format("u::rwx,g::---,o::---,u:{}:r-x,m::r-x", uid_);
    set_fd_acl(*run_home_fd_, text.c_str(), kAclAccess);
  }
  return *run_home_fd_;
}

std::vector default_blacklist = {
    ".jai",
    ".ssh",
    ".gnupg",
    ".local/share/keyrings",
    ".netrc",
    ".git-credentials",
    ".aws",
    ".azure",
    ".config/gcloud",
    ".config/gh",
    ".config/Keybase",
    ".config/kube",
    ".docker",
    ".password-store",
    ".mozilla",
    ".config/chromium",
    ".config/google-chrome",
    ".config/BraveSoftware",
    ".bash_history",
    ".zsh_history",
};

Fd
make_blacklist(int dfd, path name)
{
  Fd blacklistfd = ensure_dir(dfd, name.c_str(), 0700, kFollow);
  if (!is_dir_empty(*blacklistfd))
    return blacklistfd;

  for (path p : default_blacklist) {
    try {
      auto subdir = p.relative_path().parent_path();
      xopenat(subdir.empty()
                  ? *blacklistfd
                  : *ensure_dir(*blacklistfd, subdir, 0700, kNoFollow),
              p.filename(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    } catch (const std::exception &e) {
      std::println(stderr, "{}", e.what());
    }
  }

  return blacklistfd;
}

Fd
Config::make_home_overlay()
{
  auto restore = asuser();
  Fd changes = make_blacklist(homejai(), "changes");
  Fd work = ensure_dir(homejai(), "work", 0700, kFollow);
  restore.reset();

  Fd fsfd = fsopen("overlay", FSOPEN_CLOEXEC);
  if (!fsfd)
    syserr(R"(fsopen("overlay"))");
  if (fsconfig(*fsfd, FSCONFIG_SET_FD, "lowerdir+", nullptr, *homefd_) ||
      fsconfig(*fsfd, FSCONFIG_SET_FD, "upperdir", nullptr, *changes) ||
      fsconfig(*fsfd, FSCONFIG_SET_FD, "workdir", nullptr, *work))
    syserr("fsconfig(FSCONFIG_SET_FD)");
  Fd mnt = make_mount(*fsfd);

  Fd olhome = ensure_dir(runhome(), kSB, 0755, kFollow);
  xmnt_move(*mnt, *olhome);
  restore = asuser();
  return xopenat(runhome(), kSB, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
}

Fd
Config::make_root_dir()
{
  auto mps = mountpoints();
  int udir = runhome();
  path upath = fdpath(udir, true);

  Fd tmp;
  if (path runtmp = upath / "tmp"; !mps.contains(runtmp)) {
    tmp = make_tmpfs("gid", "0", "mode", "01777", "size", "40%");
    xmnt_move(*tmp, *ensure_dir(udir, "tmp", 0755, kNoFollow));
  }
  else
    tmp = xopenat(udir, "tmp", O_RDONLY | O_NOFOLLOW);

  Fd home;
  if (path runhome = upath / kSB; !mps.contains(runhome))
    home = make_home_overlay();
  else
    home = xopenat(udir, kSB, O_RDONLY | O_NOFOLLOW);

  Fd root = clone_tree(-1, "/", true);
  xmnt_setattr(*root,
               mount_attr{
                   .attr_set = MOUNT_ATTR_RDONLY,
                   .propagation = MS_PRIVATE,
               },
               AT_RECURSIVE);
  xmnt_move(*clone_tree(*tmp), *root, "tmp", MOVE_MOUNT_BENEATH);
  xmnt_move(*clone_tree(*tmp), *root, "var/tmp");
  Fd homeclone = clone_tree(*home);
  xmnt_propagate(*homeclone, MS_PRIVATE);
  xmnt_move(*homeclone, *root, homepath_.relative_path());
  return root;
}

static int
init_ns(void *_s)
{
  int r = 0;
  auto *s = static_cast<Config::NsState *>(_s);
  s->pipefds[1].reset();
  try {
    xmnt_propagate(*xopenat(-1, "/", O_PATH), MS_PRIVATE, true);
    xmnt_move(*s->rootfd, -1, "/mnt");
    if (chdir("/mnt"))
      syserr("/mnt");
    if (syscall(SYS_pivot_root, ".", "."))
      syserr("pivot_root");
    chdir("/");
    for (auto dir : {".", kRunRoot, "/tmp"}) {
      if (umount2(dir, MNT_DETACH))
        syserr("umount2({})", dir);
    }
  } catch (const std::exception &e) {
    r = -1;
    std::println(stderr, "{}", e.what());
    fflush(stderr);
  }
  char c;
  read(*s->pipefds[0], &c, 1);
  return r;
}

Fd
Config::make_ns()
{
  auto mps = mountpoints();
  int udir = runhome();
  path upath = fdpath(udir, true);

  Fd lock;
  for (;;) {
    path nspath = upath / "ns";
    if (mps.contains(nspath))
      return xopenat(udir, "ns", O_RDONLY);
    if (lock)
      break;
    lock = open_lockfile(udir, ".lock");
  }
  Defer _unlock([udir] { unlinkat(udir, ".lock", 0); });

  NsState s{};
  s.rootfd = make_root_dir();
  {
    int fds[2];
    if (pipe(fds))
      syserr("pipe");
    s.pipefds[0] = fds[0];
    s.pipefds[1] = fds[1];
  }

  int pid = -1;
  auto stack = std::make_unique<std::array<char, 0x10'0000>>();
  Defer reap([&pid] {
    if (pid > 0)
      while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR)
        ;
  });

  pid =
      clone(init_ns, stack->data() + stack->size(), CLONE_NEWNS | SIGCHLD, &s);

  s.pipefds[0].reset();
  Fd ns = xopenat(udir, "ns", O_CREAT | O_RDWR, 0600);
  Fd nsfs = xopenat(-1, std::format("/proc/{}/ns/mnt", pid), O_RDONLY);
  Fd mnt = clone_tree(*nsfs);
  xmnt_propagate(*mnt, MS_PRIVATE);
  xmnt_move(*mnt, *ns);
  s.pipefds[1].reset();

  int status;
  waitpid(pid, &status, 0);
  if (status) {
    umount2((upath / "ns").c_str(), MNT_DETACH);
    unlinkat(udir, "ns", 0);
    err("failed to create new namespace");
  }
  return mnt;
}

int
main(int argc, char **argv)
{
  umask(022);
  if (argc > 0)
    prog = argv[0];

#if 0
  auto mps = mountpoints();
  for (const auto &p : subtree_rev(mps, "/"))
    std::println("{}", p.string());
#endif

#if 1
  Config conf;
  conf.init();
  conf.make_ns();
#endif
}
