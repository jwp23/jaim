#include <cassert>
#include <cstring>
#include <filesystem>
#include <functional>
#include <print>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using std::filesystem::path;

path prog;

// Self-closing, movable file descriptor, use operator* to access value
struct Fd {
  int fd_{-1};

  Fd() noexcept = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  ~Fd() { reset(); }

  Fd &operator=(Fd &&other) noexcept
  {
    reset();
    fd_ = std::exchange(other.fd_, -1);
    return *this;
  }
  Fd &operator=(int fd) noexcept
  {
    reset();
    fd_ = fd;
    return *this;
  }

  int operator*() const noexcept { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }
  void reset(int fd = -1) noexcept
  {
    if (*this) {
      close(fd_);
      fd_ = fd;
    }
  }
};

// Generic RIAA destructor
struct Defer {
  std::function<void()> f_;

  Defer() noexcept = default;
  Defer(decltype(f_) f) noexcept : f_(std::move(f)) {}
  Defer(Defer &&other) noexcept : f_(other.release()) {}
  ~Defer() { reset(); }
  Defer &operator=(Defer &&other) noexcept
  {
    f_ = other.release();
    return *this;
  }
  decltype(f_) release() noexcept { return std::exchange(f_, nullptr); }
  void reset(decltype(f_) f = nullptr) noexcept
  {
    if (auto old = std::exchange(f_, f))
      old();
  }
};

struct Config {
  std::string user_;
  uid_t uid_ = -1;
  gid_t gid_ = -1;
  path homepath_;
  Fd homefd_;
  Fd jaifd_;

  void init();
  Fd makemount();
  Fd makens();

  [[nodiscard]] Defer asuser();
  Fd mkudir(int dirfd, path p, mode_t mode = 0755);
  int jaifd()
  {
    if (!jaifd_)
      jaifd_ = mkudir(*homefd_, ".jai");
    return *jaifd_;
  }
};

template<typename... Args>
[[noreturn]] void
syserr(std::format_string<Args...> fmt, Args &&...args)
{
  throw std::system_error(
      errno, std::system_category(),
      std::vformat(fmt.get(), std::make_format_args(args...)));
}

template<typename E = std::runtime_error, typename... Args>
[[noreturn]] void
err(std::format_string<Args...> fmt, Args &&...args)
{
  throw E(std::vformat(fmt.get(), std::make_format_args(args...)));
}

// Conservatively fails if file is not a regular file or cannot be
// statted for any reason.
bool
is_fd_at_path(int targetfd, int dfd, path file, bool follow = false,
              struct stat *sbout = nullptr)
{
  struct stat sbtmp, sbpath;
  if (!sbout)
    sbout = &sbtmp;
  if (fstat(targetfd, sbout))
    syserr("fstat");
  if (fstatat(dfd, file.c_str(), &sbpath, follow ? 0 : AT_SYMLINK_NOFOLLOW))
    return false;
  return sbout->st_dev == sbpath.st_dev && sbout->st_ino == sbpath.st_ino;
}

// Open an exclusive lockfile to guard one-time setup.  Might fail, in
// which case re-check the need for setup and try again.
Fd
openlock(int dfd, path file)
{
  assert(!file.empty());

  Fd fd(openat(dfd, file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
  if (fd) {
    if (!flock(*fd, LOCK_EX | LOCK_NB)) {
      struct stat sb;
      if (!is_fd_at_path(*fd, dfd, file, false, &sb))
        // Someone may have unlinked after completing setup; fail and
        // expect the invoker to call again if setup isn't complete.
        fd.reset();
      if (!S_ISREG(sb.st_mode))
        err("{}: expected regular file", file.string());
      return fd;
    }
    if (errno != EWOULDBLOCK && errno != EINTR)
      syserr(R"(flock("{}", LOCK_EX|LOCK_NB))", file.string());
    // We failed, but delay returning until lock is released, at which
    // point setup will likely be complete.
    if (flock(*fd, LOCK_SH) && errno != EINTR)
      syserr(R"(flock("{}", LOCK_SH))", file.string());
    fd.reset();
    return fd;
  }
  if (errno != ENOENT)
    syserr(R"(open("{}"))", file.string());

  path parent = file.parent_path();
  const char *pp = parent.empty() ? "." : parent.c_str();
  fd.reset(openat(dfd, pp, O_RDWR | O_TMPFILE | O_CLOEXEC, 0600));
  if (!fd)
    syserr(R"(openat("{}", O_RDWR|O_TMPFILE))", pp);
  if (flock(*fd, LOCK_EX | LOCK_NB))
    // It's a temp file so should be impossible for anyone else to lock it
    syserr("flock(O_TMPFILE)");
  if (linkat(*fd, "", dfd, file.c_str(), AT_EMPTY_PATH)) {
    if (errno != EEXIST)
      syserr(R"(linkat("{}"))", file.string());
    fd.reset();
  }
  return fd;
}

template<std::integral I>
I
parsei(const char *s)
{
  const char *const e = s + strlen(s);
  I ret{};

  auto [p, ec] = std::from_chars(s, e, ret, 10);

  if (ec == std::errc::invalid_argument || p != e)
    err<std::invalid_argument>("{}: not an integer", s);
  if (ec == std::errc::result_out_of_range)
    err<std::out_of_range>("{}: overflow", s);

  return ret;
}

bool
dir_empty(int dirfd)
{
  int fd = dup(dirfd);
  if (fd < 0)
    syserr("dup");
  auto dir = fdopendir(fd);
  if (!dir) {
    close(fd);
    syserr("fdopendir");
  }
  Defer cleanup([dir] { closedir(dir); });

  while (auto de = readdir(dir))
    if (de->d_name[0] != '.' ||
        (de->d_name[1] != '\0' &&
         (de->d_name[1] != '.' || de->d_name[2] != '\0')))
      return false;
  return true;
}

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

Fd
Config::makemount()
{
  Fd root(
      open_tree(-1, "/", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE));
  if (!root)
    syserr("open_tree(\"/\")");
  struct mount_attr ro_private = {
      .attr_set = MOUNT_ATTR_RDONLY,
      .propagation = MS_PRIVATE,
  };
  if (mount_setattr(*root, "", AT_EMPTY_PATH | AT_RECURSIVE, &ro_private,
                    sizeof(ro_private)) < 0)
    syserr("mount_setattr(root)");

  Fd tmp(fsopen("tmpfs", FSOPEN_CLOEXEC));
  if (!tmp)
    syserr("fsopen(tmpfs)");
  if (fsconfig(*tmp, FSCONFIG_SET_STRING, "size", "10%", 0))
    syserr("fsconfig(size)");
  if (fsconfig(*tmp, FSCONFIG_SET_STRING, "mode", "01777", 0))
    syserr("fsconfig(mode)");
  if (fsconfig(*tmp, FSCONFIG_CMD_CREATE, NULL, NULL, 0))
    syserr("fsconfig(CREATE)");
  Fd mnt(fsmount(*tmp, FSMOUNT_CLOEXEC, 0));
  if (!mnt)
    syserr("fsmount(tmp)");
  if (move_mount(*mnt, "", *root, "tmp", MOVE_MOUNT_F_EMPTY_PATH))
    syserr(R"(move_mount("/tmp"))");
  mnt.reset(open_tree(*root, "tmp", OPEN_TREE_CLONE));
  if (!mnt)
    syserr(R"(open_tree("/mnt/tmp"))");
  if (move_mount(*mnt, "", *root, "var/tmp", MOVE_MOUNT_F_EMPTY_PATH))
    syserr(R"(move_mount("/var/tmp"))");

  return root;
}

Fd
Config::makens()
{
  // Allocate a stack for clone, make sure we reap before freeing it.
  constexpr size_t stack_size = 0x10'0000;
  int pid = -1;
  char *const stack = reinterpret_cast<char *>(malloc(stack_size));
  Defer reap([&pid, stack] {
    if (pid > 0)
      while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR)
        ;
    free(stack);
  });

  int pipefds[2];
  if (pipe(pipefds))
    syserr("pipe");

  pid = clone(
      +[](void *pipefds) -> int {
        auto *fds = reinterpret_cast<int *>(pipefds);
        close(fds[1]);
        char c;
        while (read(fds[0], &c, 1) > 0)
          ;
        return 0;
      },
      stack + stack_size, CLONE_NEWNS | SIGCHLD, pipefds);
  int saved_errno = errno;

  close(pipefds[0]);
  Fd child_block(pipefds[1]);

  if (pid == -1) {
    errno = saved_errno;
    syserr("clone");
  }

  path child_mnt(std::format("/proc/{}/ns/mnt", pid));
  Fd nsmount(
      open_tree(-1, child_mnt.c_str(), OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC));
  if (!nsmount)
    syserr(R"(open_tree("{}"))", child_mnt.string());

  auto restore = asuser();
  Fd target(openat(jaifd(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600));
  if (!target)
    syserr("openat TMPFILE");
  if (flock(*target, LOCK_EX | LOCK_NB))
    syserr("flock TMPFILE");
  if (linkat(*target, "", jaifd(), "mnt", AT_EMPTY_PATH)) {
    if (errno == EEXIST)
      return {};
    syserr(R"(linkat(TMPFILE, "{}/.jai/mnt"))", homepath_.string());
  }
  restore.reset();

  // This won't work unless jaifd()/mnt is on an MS_PRIVATE mount
  if (move_mount(AT_FDCWD, child_mnt.c_str(), jaifd(), "mnt", 0)) {
    saved_errno = errno;
    auto x = std::format("id; ls -al {}", child_mnt.string());
    system(x.c_str());
    errno = saved_errno;
    syserr(R"(move_mount("{}", "{}/.jai/mnt"))", child_mnt.string(),
           homepath_.string());
  }

  return target;
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

Fd
Config::mkudir(int dirfd, path p, mode_t mode)
{
  auto restore = asuser();
  auto check = [this, &p](const struct stat &sb) {
    if (!S_ISDIR(sb.st_mode))
      err("{}: expected a directory", p.string());
    if (sb.st_uid != uid_)
      err("{}: expected a directory owned by {}", p.string(), user_);
    if ((sb.st_mode & 0700) != 0700)
      err("{}: expected a directory with owner rwx permissions", p.string());
  };
  struct stat sb;

  // Okay to follow symlink to existing directory owned by user
  if (Fd e{openat(dirfd, p.c_str(), O_RDONLY | O_CLOEXEC)}) {
    if (fstat(*e, &sb))
      syserr("fstat({})", p.string());
    check(sb);
    return e;
  }
  if (errno != ENOENT)
    syserr("open({})", p.string());

  if (mkdirat(dirfd, p.c_str(), mode))
    syserr("mkdir({})", p.string());
  Defer cleanup([dirfd, &p] { unlinkat(dirfd, p.c_str(), AT_REMOVEDIR); });

  Fd d{openat(dirfd, p.c_str(),
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (!d)
    syserr("open({})", p.string());

  // To void TOCTTOU bugs, make sure newly created directory is empty
  // and owned by user.
  if (!dir_empty(*d))
    err("mkudir: newly created directory {} not empty", p.string());
  if (fstat(*d, &sb))
    syserr("fstat({})", p.string());
  check(sb);

  cleanup.release();
  return d;
}

int
main(int argc, char **argv)
{
  if (argc > 0)
    prog = argv[0];

  path p{};
  std::println("parent {}, filename {}\n", p.parent_path().string(),
               p.filename().string());
  p = "x";
  std::println("parent {}, filename {}\n", p.parent_path().string(),
               p.filename().string());

  /*
  Config conf;
  conf.init();
  conf.makens();
  */
}
