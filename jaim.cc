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
 * Modified 2026 by Joseph Presley: add --file directive and claude.conf
 *   preset for coding-agent support (ja-ofy).
 * Modified 2026 by Joseph Presley: private per-invocation temp dir
 *   replaces blanket /tmp and /private/tmp allow rules; TMPDIR is
 *   rewritten in the sandboxed env (ja-4fk).
 */

#include "jaim.h"
#include "fs.h"

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <print>

#include <pwd.h>
#include <ranges>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

// macOS sandbox API (deprecated but still functional)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <sandbox.h>
#pragma clang diagnostic pop

path prog;

void
Config::parse_config_fd(int fd, Options *opts)
{
  auto ld = fdpath(fd, true);
  if (auto [_it, ok] = config_loop_detect_.insert(ld); !ok)
    err<Options::Error>("configuration loop");
  Defer _clear([this, ld, pcf = parsing_config_file_] {
    config_loop_detect_.erase(ld);
    parsing_config_file_ = pcf;
  });
  parsing_config_file_ = true;
  auto go = [&](Options *o) { o->parse_file(read_file(fd), ld); };
  go(opts ? opts : opt_parser().get());
}

bool
Config::parse_config_file(path file, Options *opts)
{
  bool slash = std::ranges::distance(file.begin(), file.end()) > 1;
  bool fromcwd = slash && !parsing_config_file_;

  if (struct stat sb;
      !slash && file.extension() != ".conf" &&
      fstatat(home_jaim(), file.c_str(), &sb, 0) && errno == ENOENT &&
      !fstatat(home_jaim(), cat(file, ".conf").c_str(), &sb, 0) &&
      S_ISREG(sb.st_mode))
    file += ".conf";

  Fd fd = openat(fromcwd ? AT_FDCWD : home_jaim(), file.c_str(), O_RDONLY);
  if (!fd) {
    if (errno == ENOENT)
      return false;
    syserr("{}", file.c_str());
  }
  parse_config_fd(*fd, opts);
  return true;
}

void
Config::init_credentials()
{
  auto realuid = getuid();

  const char *envuser{};
  if (!user_.empty())
    envuser = user_.c_str();
  else if (const char *u = getenv("SUDO_USER"))
    envuser = u;
  else if (const char *u = getenv("USER"))
    envuser = u;

  PwEnt pw;
  if (realuid == 0 && envuser) {
    if (!(pw = PwEnt::get_nam(envuser)))
      err("cannot find password entry for user {}", envuser);
  }
  else if (!(pw = PwEnt::get_id(realuid)))
    err("cannot find password entry for uid {}", realuid);

  user_ = pw->pw_name;
  homepath_ = path("/");
  homepath_ /= pw->pw_dir;
  shell_ = pw->pw_shell;
  user_cred_ = Credentials::get_user(pw);

  setenv("JAIM_USER", user_.c_str(), 1);

  const char *jcd = getenv("JAIM_CONFIG_DIR");
  homejaimpath_ = homepath_ / (jcd ? jcd : ".jaim");
  setenv("JAIM_CONFIG_DIR", homejaimpath_.c_str(), 1);

  old_umask_ = umask(0);
  umask(old_umask_);
}

void
Config::check_user(const struct stat &sb, std::string p)
{
  if (sb.st_uid != user_cred_.uid_)
    err("{}: owned by {} should be owned by {}", p, sb.st_uid,
        user_cred_.uid_);
}

int
Config::home_jaim(bool create)
{
  if (!home_jaim_fd_) {
    if (create)
      home_jaim_fd_ = ensure_udir(home(), homejaimpath_);
    else if (Fd fd = openat(home(), homejaimpath_.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
      check_user(*fd);
      home_jaim_fd_ = std::move(fd);
    }
    else if (errno == ENOENT) {
      err("{} does not exist; run {} --init to create it",
          fdpath(home(), homejaimpath_), prog.filename().string());
    }
    else
      syserr("{}", fdpath(home(), homejaimpath_));
  }
  return *home_jaim_fd_;
}

int
Config::storage()
{
  if (storage_fd_)
    return *storage_fd_;

  if (storagedir_.empty())
    storage_fd_ = xdup(home_jaim());
  else
    storage_fd_ = ensure_udir(AT_FDCWD, storagedir_);

  path fullpath = fdpath(*storage_fd_, true);
  if (fullpath.is_relative())
    err("cannot find full pathname for {}", storagedir_.string());
  if (!is_fd_at_path(*storage_fd_, -1, fullpath))
    err("{} is no longer at {}", storagedir_.string(), fullpath.string());
  storagedir_ = fullpath;

  return *storage_fd_;
}

int
Config::home()
{
  if (!home_fd_) {
    Fd fd;
    if (!(fd = open(homepath_.c_str(), O_RDONLY | O_CLOEXEC)))
      syserr("{}", homepath_.string());
    check_user(*fd);
    home_fd_ = std::move(fd);
  }
  return *home_fd_;
}

void
Config::init_jail(int newhomefd)
{
  if (jaiminit_.empty())
    return;
  if (access(jaiminit_.c_str(), X_OK))
    syserr("{}", jaiminit_.string());
  if (auto pid = xfork()) {
    int status;
    while (waitpid(pid, &status, 0) == -1)
      if (errno != EINTR)
        syserr("jaiminit waitpid");
    if (WIFEXITED(status)) {
      if (auto val = WEXITSTATUS(status))
        warn("{}: exit code {}{}", jaiminit_.string(), val,
             val == 199 ? " (probably couldn't execute)" : "");
    }
    else if (WIFSIGNALED(status))
      warn("{}: killed by signal {}", jaiminit_.string(), WTERMSIG(status));
    return;
  }

  try {
    if (fchdir(newhomefd))
      syserr("{}", fdpath(newhomefd));
    umask(old_umask_);
    execl(jaiminit_.c_str(), jaiminit_.c_str(), nullptr);
    syserr("{}", jaiminit_.string());
  } catch (const std::exception &e) {
    warn("{}", e.what());
    _exit(199);
  }
}

// Escape a path string for use in a Seatbelt profile.
// Seatbelt uses Scheme-like syntax; quotes and backslashes need escaping.
static std::string
sbpl_escape(const std::string &s)
{
  std::string ret;
  ret.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\')
      ret += '\\';
    ret += c;
  }
  return ret;
}

// Escape a path so it can be embedded literally inside a Seatbelt
// regex.  Metacharacters get a backslash prefix so the path matches
// only itself, not whatever pattern the character would otherwise
// mean.  Seatbelt's #"..." regex literal is a raw string (backslashes
// pass through to the regex engine unchanged) so the caller only has
// to escape embedded quotes separately.
static std::string
sbpl_regex_escape(const std::string &s)
{
  std::string ret;
  ret.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
    case '.': case '*': case '+': case '?': case '^': case '$':
    case '(': case ')': case '[': case ']': case '{': case '}':
    case '|': case '\\':
      ret += '\\';
      [[fallthrough]];
    default:
      ret += c;
    }
  }
  return ret;
}

std::string
Config::generate_sandbox_profile()
{
  std::string p;

  p += "(version 1)\n";
  p += "(deny default)\n\n";

  // Basic process operations
  p += "(allow process*)\n";
  p += "(allow signal)\n";
  p += "(allow sysctl-read)\n\n";

  // Mach and IPC (needed for basic macOS functionality)
  p += "(allow mach*)\n";
  p += "(allow ipc-posix*)\n";
  p += "(allow iokit*)\n\n";

  // Network access (jaim doesn't restrict network, same as jai)
  p += "(allow network*)\n\n";

  // Pseudo-terminals (needed for interactive shells)
  p += "(allow pseudo-tty)\n\n";

  // System read access
  p += "(allow file-read*\n";
  p += "  (subpath \"/usr\")\n";
  p += "  (subpath \"/bin\")\n";
  p += "  (subpath \"/sbin\")\n";
  p += "  (subpath \"/Library\")\n";
  p += "  (subpath \"/System\")\n";
  p += "  (subpath \"/etc\")\n";
  p += "  (subpath \"/private/etc\")\n";
  p += "  (subpath \"/private/var\")\n";
  p += "  (subpath \"/dev\")\n";
  p += "  (subpath \"/opt/homebrew\")\n";
  p += "  (subpath \"/Applications\")\n";
  p += "  (subpath \"/nix\")\n";
  p += "  (literal \"/\")\n";
  p += "  (literal \"/private\")\n";
  p += "  (literal \"/tmp\")\n";
  p += "  (literal \"/var\"))\n\n";

  // Allow read/write to /dev/null, /dev/tty, and PTY devices
  p += "(allow file* (literal \"/dev/null\"))\n";
  p += "(allow file* (literal \"/dev/tty\"))\n";
  p += "(allow file* (regex #\"^/dev/ttys[0-9]+$\"))\n";
  p += "(allow file* (literal \"/dev/ptmx\"))\n\n";

  // Private temp directory.  jaim creates a fresh directory per
  // invocation (see Config::exec) and points TMPDIR at it, so the
  // sandboxed process has writable scratch space without handing it
  // the shared system /tmp, /private/tmp, or /var/folders tree.
  // Those system-wide locations stay denied by the default-deny rule
  // at the top of this profile, which blocks leaks between concurrent
  // sandboxes and exposure of files dropped by unsandboxed processes.
  if (private_tmp_.empty())
    err<std::logic_error>("generate_sandbox_profile: private_tmp_ unset");
  p += std::format("(allow file* (subpath \"{}\"))\n\n",
                   sbpl_escape(private_tmp_.string()));

  // Mode-specific home directory access
  if (mode_ == kCasual || mode_ == kBare) {
    // Casual and Bare: read-only access to home directory, excluding
    // masked paths.  Writes go through the CWD allow rule below (which
    // grants file*), or through explicit --dir / --rdir grants.  jaim
    // has no overlay filesystem, so granting file* on $HOME would
    // touch real files on disk — contradicting the README's promise
    // that casual mode protects against accidental deletion.  Use
    // require-all with require-not to properly exclude masked paths,
    // since Seatbelt's allow rules take precedence over later deny
    // rules.
    const char *access = "file-read*";
    if (mask_files_.empty()) {
      p += std::format("(allow {} (subpath \"{}\"))\n",
                       access, sbpl_escape(homepath_.string()));
    }
    else {
      p += std::format("(allow {}\n", access);
      p += "  (require-all\n";
      p += std::format("    (subpath \"{}\")\n",
                       sbpl_escape(homepath_.string()));
      for (const auto &m : mask_files_) {
        auto masked = homepath_ / m;
        p += std::format("    (require-not (subpath \"{}\"))\n",
                         sbpl_escape(masked.string()));
      }
      p += "  ))\n";
    }
    p += "\n";

    // SSH allow-list carve-outs: when the entire ~/.ssh tree is masked
    // (the default, see default_conf.cc), re-allow the specific files
    // that SSH clients and related tooling normally need.  Deny-by-
    // default under ~/.ssh keeps privately-named key files out of
    // reach regardless of their naming convention; enumerating denies
    // does not.
    if (mask_files_.contains(path(".ssh"))) {
      auto ssh = homepath_ / ".ssh";
      auto ssh_str = sbpl_escape(ssh.string());
      // SSH agent sockets (directory subpath so the whole agent dir
      // including per-session sockets is reachable).
      p += std::format("(allow {} (subpath \"{}/agent\"))\n",
                       access, ssh_str);
      // Host key database.
      p += std::format("(allow {} (literal \"{}/known_hosts\"))\n",
                       access, ssh_str);
      p += std::format("(allow {} (literal \"{}/known_hosts.old\"))\n",
                       access, ssh_str);
      // Public keys — readable from sandboxed tooling (git, ssh-add
      // -L, etc.), but not writable, since modifying a .pub file
      // does not require sandbox write access.  Seatbelt's #"..."
      // regex literal is a raw string: backslashes pass through to
      // the regex engine unchanged, so we emit single backslashes
      // only.  Quotes inside the regex still have to be escaped.
      auto pub_regex = std::format("^{}/[^/]+\\.pub$",
                                   sbpl_regex_escape(ssh.string()));
      std::string pub_regex_quoted;
      for (char c : pub_regex) {
        if (c == '"')
          pub_regex_quoted += '\\';
        pub_regex_quoted += c;
      }
      p += std::format("(allow file-read* (regex #\"{}\"))\n",
                       pub_regex_quoted);
      p += "\n";
    }
  }
  // Strict mode: no home directory access by default (deny default handles it)

  // CWD access - full read/write, with the same mask exclusions as the
  // home rule.  Seatbelt unions allow rules, so emitting an unscoped
  // (allow file* (subpath CWD)) would nullify every mask whose full
  // path sits inside CWD — casual-mode users running jaim from $HOME
  // (the README's default pattern) would silently expose .ssh, .aws,
  // .gnupg, shell histories, browser data, and every other masked
  // path.  The require-not clauses fix the policy; the pre-check
  // below refuses to start outright when CWD is itself inside a
  // masked path, because in that case the require-not on the CWD
  // subpath nullifies the rule entirely (chdir would fail and the
  // user would see a confusing error) — better to fail early with
  // clear guidance.
  if (grant_cwd_) {
    const auto &c = cwd();
    for (const auto &m : mask_files_) {
      auto masked = homepath_ / m;
      if (contains(masked, c))
        err("refusing to grant CWD access: {} is inside masked path {}.\n"
            "  Use -D/--nocwd to disable CWD access, or cd to a different "
            "directory.",
            c.string(), masked.string());
    }
    if (mask_files_.empty()) {
      p += std::format("(allow file* (subpath \"{}\"))\n",
                       sbpl_escape(c.string()));
    }
    else {
      p += "(allow file*\n";
      p += "  (require-all\n";
      p += std::format("    (subpath \"{}\")\n", sbpl_escape(c.string()));
      for (const auto &m : mask_files_) {
        auto masked = homepath_ / m;
        p += std::format("    (require-not (subpath \"{}\"))\n",
                         sbpl_escape(masked.string()));
      }
      p += "  ))\n";
    }
  }

  // Granted directories
  for (const auto &[d, flags] : grant_directories_) {
    auto dp = sbpl_escape(d.string());
    if (flags & kGrantRO)
      p += std::format("(allow file-read* (subpath \"{}\"))\n", dp);
    else
      p += std::format("(allow file* (subpath \"{}\"))\n", dp);
  }

  // File grants: atomic-write aware.  Each `file PATH` grant emits
  // two rules:
  //   1. A literal match for PATH itself.
  //   2. A regex match for `PATH.<suffix>` siblings used by atomic-
  //      write libraries (write-file-atomic in node, atomicwrites
  //      in Python, tempfile + rename in most others).  Those
  //      libraries write to a temp sibling then rename() it into
  //      place, which requires file-write ops on BOTH the temp and
  //      the target — the literal rule handles the target, the
  //      sibling regex handles the temp.
  // A single combined regex with an optional group would be terser,
  // but Seatbelt's regex engine rejects some POSIX ERE constructs
  // (observed: `(\.[A-Za-z0-9_-]+)?` fails to parse), so we split
  // the two cases into separate rules.  This is still tighter than
  // granting the whole parent directory.
  for (const auto &[f, flags] : grant_files_) {
    const char *rule = (flags & kGrantRO) ? "file-read*" : "file*";
    p += std::format("(allow {} (literal \"{}\"))\n",
                     rule, sbpl_escape(f.string()));
    // Use `.+` rather than an explicit character class for the
    // suffix: Seatbelt's regex engine appears to mis-parse bracket
    // expressions that immediately follow `\.` (observed: it drops
    // the leading `[` and reports "unterminated bracket expression"
    // on the remainder), so we let any non-empty suffix match.
    // Atomic-write libraries pick the suffix; we only need the
    // prefix anchor for security.
    auto pattern = std::format("^{}\\..+$",
                               sbpl_regex_escape(f.string()));
    std::string quoted;
    quoted.reserve(pattern.size());
    for (char c : pattern) {
      if (c == '"')
        quoted += '\\';
      quoted += c;
    }
    p += std::format("(allow {} (regex #\"{}\"))\n", rule, quoted);
  }

  return p;
}

path
Config::make_script()
{
  if (script_inputs_.empty())
    return {};

  // The script lives in the per-invocation private temp dir so the
  // sandboxed child can unlink it after sourcing (see the rm trailer
  // written below).  The old location — the real TMPDIR inherited
  // from the parent — is no longer reachable from inside the sandbox.
  if (private_tmp_.empty())
    err<std::logic_error>("make_script: private_tmp_ unset");
  path tmpdir = private_tmp_;

  // Generate random filename
  std::array<unsigned char, 10> rndbuf;
  arc4random_buf(rndbuf.data(), rndbuf.size());

  path fname = ".jaimrc";
  for (auto i : rndbuf)
    fname +=
        "+0123456789=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            [i & 0x3f];

  path fullpath = tmpdir / fname;
  Fd w = xopenat(-1, fullpath, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0400);

  auto dowrite = [&w](std::string_view sv) {
    auto *p = sv.data(), *e = p + sv.size();
    while (p < e)
      if (auto n = write(*w, p, e - p); n > 0)
        p += n;
      else {
        assert(n == -1);
        syserr("write scriptfile");
      }
  };

  for (const auto &input : script_inputs_) {
    dowrite(read_file(-1, input));
    dowrite("\n");
  }
  dowrite(R"(
# Remove $JAIM_SCRIPT once sourced unless JAIM_KEEP_SCRIPT is set
if [[ -n $JAIM_SCRIPT && -z ${JAIM_KEEP_SCRIPT+set} ]]; then
    rm -f "$JAIM_SCRIPT"
    unset JAIM_SCRIPT
fi
)");

  return fullpath;
}

std::vector<const char *>
Config::make_env()
{
  std::vector<std::string_view> filter_patterns;
  std::set<std::string_view, std::less<>> filter_vars;
  for (const auto &v : env_filter_)
    if (v.find('*') == v.npos)
      filter_vars.insert(v);
    else
      filter_patterns.push_back(v);

  for (char **e = environ; *e; ++e) {
    std::string_view sv(*e);
    if (auto eq = sv.find('='); eq != sv.npos)
      sv = sv.substr(0, eq);
    else
      continue;
    if (filter_vars.contains(sv) ||
        std::ranges::any_of(filter_patterns,
                            [sv](auto pat) { return glob(pat, sv); }))
      continue;
    setenv_.try_emplace(std::string(sv), *e);
  }

  auto env_view =
      setenv_ | std::views::values | std::views::transform(&std::string::c_str);
  std::vector<const char *> ret(env_view.begin(), env_view.end());
  ret.push_back(nullptr);
  return ret;
}

// Return stop signal if status indicates a child stopped, exit or
// kill ourselves if the child terminated on a signal, and return 0
// otherwise.
static pid_t main_pid = getpid();

template<typename AtExit = void (*)()>
static int
propagate_termination_status(int status, AtExit &&atexit = +[] {})
{
  if (WIFSTOPPED(status)) {
    if (int sig = WSTOPSIG(status);
        sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU)
      return sig;
    return 0;
  }

  auto do_exit = [&atexit](int status) {
    atexit();
    (getpid() == main_pid ? exit : _exit)(status);
  };

  if (WIFEXITED(status))
    do_exit(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    signal(sig, SIG_DFL);
    auto ss = sigsingleton(WTERMSIG(status));
    sigprocmask(SIG_UNBLOCK, &ss, nullptr);
    raise(sig);
    do_exit(-1);
  }

  return 0; // Continued?
}

void
Config::exec(char **argv)
{
  // Per-invocation private temp directory.  Created first because
  // make_script() drops the JAIM_SCRIPT file here, generate_sandbox_profile()
  // embeds the resolved path in its allow rule, and make_env() in the
  // child points TMPDIR at it.  canonical() is needed because the
  // kernel enforces the sandbox against resolved paths — a profile
  // that allowed /tmp/jaim.XYZ but not /private/tmp/jaim.XYZ would
  // deny every access on macOS, where /tmp is a symlink.
  {
    path base = std::filesystem::temp_directory_path();
    std::string templ = (base / "jaim.XXXXXXXX").string();
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data()))
      syserr("mkdtemp({})", templ);
    private_tmp_ = canonical(path(buf.data()));
  }
  // Force TMPDIR in the sandboxed env to the private dir.  Using
  // insert_or_assign (rather than try_emplace) deliberately overrides
  // any TMPDIR the user exported or set with --setenv: the sandbox
  // denies writes everywhere else, so honoring a user-supplied TMPDIR
  // would just break every program that uses $TMPDIR.
  setenv_.insert_or_assign(
      "TMPDIR", std::format("TMPDIR={}", private_tmp_.string()));

  auto script_path = make_script();

  // Generate the sandbox profile
  auto profile = generate_sandbox_profile();

  auto pid = xfork();
  if (!pid) {
    // Child process
    try {
      // Apply sandbox profile before exec
      char *errorbuf = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      if (sandbox_init(profile.c_str(), 0, &errorbuf) != 0) {
        std::string errmsg = errorbuf ? errorbuf : "unknown error";
        if (errorbuf)
          sandbox_free_error(errorbuf);
        err("sandbox_init: {}", errmsg);
      }
#pragma clang diagnostic pop

      if (!script_path.empty())
        setenv("JAIM_SCRIPT", script_path.c_str(), 1);

      if (chdir(cwd().c_str()))
        syserr("chdir({})", cwd().string());

      umask(old_umask_);

      const char *argv0 = argv[0];
      std::vector<const char *> bashcmd;
      if (!shellcmd_.empty()) {
        argv0 = PATH_BASH;
        bashcmd.push_back("init");
        bashcmd.push_back("-c");
        bashcmd.push_back(shellcmd_.c_str());
        while (*argv)
          bashcmd.push_back(*(argv++));
        bashcmd.push_back(nullptr);
        argv = const_cast<char **>(bashcmd.data());
      }

      auto env = make_env();
      execve(argv0, argv, const_cast<char **>(env.data()));
      // If execve fails, try execvp as fallback for PATH lookup
      execvp(argv0, argv);
      perror(argv0);
      _exit(1);
    } catch (const std::exception &e) {
      warn("{}", e.what());
      _exit(1);
    }
  }

  // Parent process: wait for child, propagate termination status.
  // atexit_fn removes the private temp directory (which includes the
  // generated script, if any).  Best-effort: a signal interrupting
  // the parent can still leak the directory, so don't treat errors
  // as fatal — jaim -u (planned, ja-8bh) will sweep up stragglers.
  std::function<void()> atexit_fn = [this] {
    if (!private_tmp_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(private_tmp_, ec);
    }
  };

  for (;;) {
    int status;
    if (auto r = waitpid(pid, &status, WUNTRACED); r == -1) {
      if (errno != EINTR) {
        atexit_fn();
        syserr("waitpid");
      }
    }
    else if (r == pid) {
      if (auto sig = propagate_termination_status(status, atexit_fn); sig > 0)
        raise(sig);
    }
  }
}

std::unique_ptr<Options>
Config::opt_parser(bool dotjail)
{
  auto ret = std::make_unique<Options>();
  Options &opts = *ret;
  opts(
      "-m", "--mode",
      [this](std::string_view m) {
        static const std::map<std::string, Mode, std::less<>> modemap{
            {"casual", kCasual}, {"bare", kBare}, {"strict", kStrict}};
        if (auto it = modemap.find(m); it != modemap.end())
          mode_ = it->second;
        else
          err<Options::Error>(R"(invalid mode {})", m);
      },
      R"(Set execution mode to one of the following:
    casual - sandbox with read access to home directory
    bare - sandbox with read access to home directory
    strict - sandbox with no home directory access)",
      "casual|bare|strict");
  opts(
      "-d", "--dir",
      [this](std::string_view arg) {
        path d(expand(arg));
        grant_directories_.emplace(
            canonical(parsing_config_file_ ? homepath_ / d : d), 0);
      },
      "Grant full access to DIR", "DIR");
  opts(
      "--dir!",
      [this](std::string_view arg) {
        path d(expand(arg));
        grant_directories_.emplace(
            weakly_canonical(parsing_config_file_ ? homepath_ / d : d),
            kGrantMkdir);
      },
      "Like --dir, but create DIR if it doesn't exist", "DIR");
  opts(
      "-r", "--rdir",
      [this](std::string_view arg) {
        path d(expand(arg));
        grant_directories_.emplace(
            canonical(parsing_config_file_ ? homepath_ / d : d), kGrantRO);
      },
      "Grant read-only access to DIR", "DIR");
  opts(
      "--rdir?",
      [this](std::string_view arg) {
        path d(expand(arg));
        try {
          grant_directories_.emplace(
              canonical(parsing_config_file_ ? homepath_ / d : d), kGrantRO);
        } catch (const std::exception &) {
        }
      },
      "Like --rdir but ignore the option if DIR does not exist", "DIR");
  opts(
      "-x", "--xdir",
      [this](std::string_view arg) {
        path d(expand(arg));
        grant_directories_.erase(
            canonical(parsing_config_file_ ? homepath_ / d : d));
      },
      "Undo the effects of a previous --dir option", "DIR");
  opts(
      "-F", "--file",
      [this](std::string_view arg) {
        path p(expand(arg));
        // weakly_canonical so the grant works for files that do not
        // exist yet (e.g. ~/.claude.json before the first Claude Code
        // run creates it).  Seatbelt does not require the path to
        // exist at profile generation time.
        grant_files_.emplace(
            weakly_canonical(parsing_config_file_ ? homepath_ / p : p), 0);
      },
      "Grant write access to FILE (and atomic-write .<suffix> siblings)",
      "FILE");
  opts(
      "--xfile",
      [this](std::string_view arg) {
        path p(expand(arg));
        grant_files_.erase(
            weakly_canonical(parsing_config_file_ ? homepath_ / p : p));
      },
      "Undo the effects of a previous --file option", "FILE");
  opts(
      "-D", "--nocwd", [this] { grant_cwd_ = false; },
      "Do not grant access to the current working directory");
  if (!dotjail)
    opts(
        "-j", "--jail",
        [this](path sb) {
          if (!name_ok(sb))
            err<Options::Error>("{}: invalid sandbox name", sb.string());
          sandbox_name_ = sb;
        },
        "Use sandbox named NAME", "NAME");
  else
    opts("-j", "--jail", [](path) {
      err<Options::Error>("cannot set name from a .jail file or include");
    });
  opts("--conf", [this, opts = ret.get()](std::string_view arg) {
    path file(expand(arg));
    if (!parse_config_file(file, opts))
      err<Options::Error>("{}: configuration file not found", file.string());
  });
  opts("--conf?", [this, opts = ret.get()](std::string_view arg) {
    parse_config_file(expand(arg), opts);
  });
  opts(
      "--script",
      [this](path arg) {
        arg = canonical(parsing_config_file_ ? homejaimpath_ / arg : arg);
        if (!std::ranges::contains(script_inputs_, arg))
          script_inputs_.push_back(std::move(arg));
      },
      "Source SCRIPT in bash shell used to launch jail", "SCRIPT");
  opts(
      "--script?",
      [this](path arg) {
        try {
          arg = canonical(parsing_config_file_ ? homejaimpath_ / arg : arg);
          if (!std::ranges::contains(script_inputs_, arg))
            script_inputs_.push_back(std::move(arg));
        } catch (const std::exception &) {
        }
      },
      "Like --script but don't fail if SCRIPT does not exist", "SCRIPT");
  opts(
      "--initjail",
      [this](path arg) {
        jaiminit_ = canonical(parsing_config_file_ ? homejaimpath_ / arg : arg);
        if (access(jaiminit_.c_str(), X_OK))
          err<Options::Error>("{}: {}", jaiminit_.string(),
                              errno == EACCES ? "no execute permission"
                                              : strerror(errno));
      },
      "Run PROGRAM to initialize new home directories", "PROGRAM");
  opts(
      "--initjail?",
      [this](path arg) {
        arg = weakly_canonical(parsing_config_file_ ? homejaimpath_ / arg : arg);
        if (access(arg.c_str(), X_OK)) {
          if (errno == ENOENT)
            return;
          else
            err<Options::Error>("{}: {}", arg.string(),
                                errno == EACCES ? "no execute permission"
                                                : strerror(errno));
        }
        jaiminit_ = arg;
      },
      "Like --initjail, but silently ignore non-existent PROGRAM", "PROGRAM");
  opts(
      "--mask",
      [this](std::string_view arg) {
        path p(expand(arg));
        if (p.is_absolute())
          err<Options::Error>("{}: cannot mask an absolute path", p.string());
        mask_files_.emplace(std::move(p));
      },
      "Deny sandbox access to $HOME/FILE", "FILE");
  opts(
      "--unmask",
      [this](std::string_view arg) {
        path p(expand(arg));
        mask_files_.erase(p);
      },
      "Undo the effects of a previous --mask option", "FILE");
  opts(
      "--unsetenv",
      [this](std::string_view var) {
        erase_if(setenv_,
                 [var](const auto &it) { return glob(var, it.first); });
        env_filter_.emplace(var);
      },
      "Remove VAR (which may contain wildcard '*') from the environment",
      "VAR");
  opts(
      "--setenv",
      [this](std::string var) {
        if (auto pos = var.find('='); pos != var.npos) {
          auto name = var.substr(0, pos);
          auto var_eq_val = std::format("{}{}", var.substr(0, pos + 1),
                                        expand(var.substr(pos + 1)));
          // setenv VAR=VALUE unconditionally overrides any prior unsetenv
          // (including wildcard patterns that would otherwise have stripped
          // VAR).  When this happens inside a config file, warn — it is
          // almost certainly a footgun: `setenv FOO=${FOO}` captures the
          // real-environment value of FOO at parse time and then feeds it
          // into the sandbox, silently defeating the default credential
          // strip.  The warning is suppressible by naming the variable
          // outside any unsetenv pattern.
          if (parsing_config_file_)
            if (auto it = std::ranges::find_if(
                    env_filter_,
                    [&name](const auto &pat) { return glob(pat, name); });
                it != env_filter_.end())
              warn("warning: setenv {} overrides unsetenv {} — variable will "
                   "be passed into the sandbox",
                   name, *it);
          setenv_.insert_or_assign(std::string(name), var_eq_val);
        }
        else if (auto it = env_filter_.find(var); it != env_filter_.end())
          env_filter_.erase(it);
        else if (var.contains(' '))
          err<Options::Error>(
              R"(Environment variable "{}" contains space, did you mean '='?)",
              var);
        else if (const char *p = getenv(var.c_str());
                 p && std::ranges::any_of(env_filter_, [&var](const auto &pat) {
                   return glob(pat, var);
                 }))
          setenv_.insert_or_assign(var, std::format("{}={}", var, p));
      },
      "Undo the effects of --unsetenv=VAR, or set VAR=VALUE", "VAR[=VALUE]");
  opts(
      "--command", [this](std::string cmd) { shellcmd_ = std::move(cmd); },
      R"(Bash command line to execute program, e.g:
source "${JAIM_SCRIPT:-/dev/null}"; "$0" "$@")",
      "CMD");
  opts(
      "--storage",
      [this](std::string_view s) {
        auto sd = expand(s);
        if (sd.empty())
          storagedir_ = path{};
        else if (parsing_config_file_)
          storagedir_ = homepath_ / sd;
        else
          storagedir_ = cwd() / sd;
      },
      R"(Store sandbox state in DIR
(default: $JAIM_CONFIG_DIR or $HOME/.jaim))",
      "DIR");
  return ret;
}

std::string option_help;

[[noreturn]] static void
usage(int status)
{
  if (status)
    std::println(stderr, "Try {} --help for more information.",
                 prog.filename().string());
  else
    std::print(stdout, "usage: {0} [OPTIONS] [CMD [ARG...]]\n{1}",
               prog.filename().string(), option_help);
  exit(status);
}

[[noreturn]] static void
version()
{
  std::println(R"({}
{}

Copyright (C) 2026 David Mazieres (original jai)
macOS port: jaim

This program comes with NO WARRANTY, to the extent permitted by law.
You may redistribute it under the terms of the GNU General Public License
version 3 or later; see the file named COPYING for details.)",
               PACKAGE_STRING, PACKAGE_URL);
  exit(0);
}

int
do_main(int argc, char **argv)
{
  Config conf;
  conf.init_credentials();

  path opt_C = "";
  bool opt_C_optional{};
  bool opt_init{};

  auto opts = conf.opt_parser();
  (*opts)(
      "--init", [&] { opt_init = true; },
      "Create initial configuration files and exit");
  (*opts)(
      "-C", "--conf",
      [&](path p) {
        opt_C = p;
        opt_C_optional = false;
      },
      R"(Use FILE as configuration file.  A file FILE with no '/'
is relative to $JAIM_CONFIG_DIR if set, otherwise to ~/.jaim.
The default is CMD.conf if it exists, otherwise default.conf)",
      "FILE");
  (*opts)(
      "--conf?",
      [&](path p) {
        opt_C = p;
        opt_C_optional = true;
      },
      R"(Like --conf, but no error if the file does not exist)", "FILE");
  (*opts)("--help", [] { usage(0); });
  (*opts)("--version", version, "Print copyright and version then exit");
  (*opts)(
      "--print-defaults",
      [] {
        write(1, jaim_defaults.data(), jaim_defaults.size());
        exit(0);
      },
      "Show default contents of $JAIM_CONFIG_DIR/.defaults");
  option_help = opts->help();

  if (argc > 2 && !strcmp(argv[1], "--complete"))
    return conf.complete(opts->complete_args(2, argc, argv));

  std::vector<char *> cmd;
  try {
    auto parsed = opts->parse_argv(argc, argv);
    cmd.assign(parsed.begin(), parsed.end());
  } catch (Options::Error &e) {
    warn("{}", e.what());
    usage(2);
  }

  // Compute and cache pwd after early-exit options (--help, --version)
  // have been handled, since canonical() can fail under sandbox.
  setenv("PWD", conf.cwd().c_str(), 1);

  ensure_file(conf.home_jaim(true), ".defaults", jaim_defaults, 0600,
              create_warn);
  ensure_file(conf.home_jaim(), "default.conf", default_conf, 0600, create_warn);
  ensure_file(conf.home_jaim(), ".jaimrc", default_jaimrc, 0600, create_warn);
  ensure_file(conf.home_jaim(), "claude.conf", default_claude_conf, 0600,
              create_warn);

  if (opt_init) {
    ensure_file(conf.storage(), "default.jail", default_jail, 0600,
                create_warn);
    std::println("You can edit the configuration defaults in {}/.defaults.",
                 conf.homejaimpath_.string());
    std::println(
        "Run {} --print-defaults to see the original contents of that file.",
        prog.filename().string());
    return 0;
  }

  if (!opt_C.empty()) {
    if (!conf.parse_config_file(opt_C))
      err("{}: no such configuration file", opt_C.string());
  }
  else if ((cmd.empty() || !conf.name_ok(cmd[0]) ||
            !conf.parse_config_file(std::format("{}.conf", cmd[0]))) &&
           !conf.parse_config_file("default.conf"))
    conf.parse_config_file("default.conf");

  opts->parse_argv(argc, argv);

  if (conf.sandbox_name_.empty())
    conf.sandbox_name_ = "default";

  Fd dotjail = ensure_file(conf.storage(), cat(conf.sandbox_name_, ".jail"),
                           conf.sandbox_name_ == "default"
                               ? default_jail
                               : std::format("mode {}\n", conf.mode_),
                           0600, create_warn);
  conf.parse_config_fd(*dotjail, conf.opt_parser(true).get());

  opts->parse_argv(argc, argv);

  setenv("JAIM_JAIL", conf.sandbox_name_.c_str(), 1);
  setenv("JAIM_MODE", std::format("{}", conf.mode_).c_str(), 1);

  if (cmd.empty()) {
    const char *shell = conf.shell_.empty() ? "/bin/sh" : conf.shell_.c_str();
    cmd.push_back(const_cast<char *>(shell));
  }

  cmd.push_back(nullptr);
  conf.exec(cmd.data());
  return 0;
}

int
main(int argc, char **argv)
{
  if (argc > 0)
    prog = argv[0];
  else
    prog = PACKAGE_TARNAME;

#if 1
  using ToCatch = std::exception;
#else
  struct ToCatch {
    auto what() const { return ""; }
  };
#endif

  try {
    exit(do_main(argc, argv));
  } catch (const ToCatch &e) {
    warn("{}", e.what());
  }
  return 1;
}
