// -*-C++-*-
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
 */

#pragma once

#include "config.h"
#include "cred.h"
#include "err.h"
#include "fs.h"
#include "options.h"

#include <signal.h>
#include <unistd.h>

extern "C" char **environ;

inline const char *
env_or_empty(std::string_view var)
{
  const char *p = getenv(std::string(var).c_str());
  return p ? p : "";
}

// Calls exp("VAR"sv) to expand strings like "123${VAR}456".
template<typename Exp = decltype(env_or_empty)>
std::string
var_expand(std::string_view in, Exp &&exp = env_or_empty)
    requires requires(std::string r) { r += exp(in); }
{
  std::string ret;
  for (std::size_t i = 0, e = in.size(); i < e;)
    if (in[i] == '\\')
      ret += (++i < e ? in[i++] : '\\');
    else if (size_t j;
             in.substr(i, 2) == "${" && (j = in.find('}', i + 2)) != in.npos) {
      ret += exp(in.substr(i + 2, j - i - 2));
      i = j + 1;
    }
    else
      ret += in[i++];
  return ret;
}

inline pid_t
xfork()
{
  auto ret = fork();
  if (ret == -1)
    syserr("fork");
  return ret;
}

inline sigset_t
sigsingleton(int sig)
{
  sigset_t ret;
  sigemptyset(&ret);
  sigaddset(&ret, sig);
  return ret;
}

extern const std::string jaim_defaults;
extern const std::string default_conf;
extern const std::string default_jail;
extern const std::string default_jaimrc;
extern const std::string default_claude_conf;

struct Config {
  enum Mode { kCasual, kBare, kStrict };
  static constexpr int kGrantRO = 1 << 0;
  static constexpr int kGrantMkdir = 1 << 1;

  Mode mode_{kStrict};
  PathMap<int> grant_directories_;
  PathMap<int> grant_files_;
  bool grant_cwd_{true};
  std::set<std::string, std::less<>> env_filter_;
  std::map<std::string, std::string, std::less<>> setenv_;
  path cwd_;
  std::vector<path> script_inputs_;
  std::string shellcmd_;
  PathSet mask_files_;
  bool parsing_config_file_{};

  std::string user_;
  path homepath_;
  path homejaimpath_;
  path storagedir_;
  path sandbox_name_;
  path jaiminit_;
  Credentials user_cred_;
  path shell_;
  mode_t old_umask_ = 0755;

  Fd home_fd_;
  Fd home_jaim_fd_;
  Fd storage_fd_;

  PathSet config_loop_detect_;

  void init_credentials();
  std::string generate_sandbox_profile();
  path make_script();
  void exec(char **argv);
  std::unique_ptr<Options> opt_parser(bool dotjail = false);

  int complete(Options::Completions c);
  void parse_config_fd(int fd, Options *opts = nullptr);
  bool parse_config_file(path file, Options *opts = nullptr);
  std::vector<const char *> make_env();
  void init_jail(int newhomefd);

  void check_user(const struct stat &sb, std::string path_for_error = {});
  void check_user(int fd, std::string path_for_error = {})
  {
    check_user(xfstat(fd), path_for_error.empty() ? fdpath(fd) : path_for_error);
  }
  Fd ensure_udir(
      int dfd, const path &p, mode_t perm = 0700, FollowLinks follow = kFollow,
      CreateCB createcb = [](int) {})
  {
    Fd fd = ensure_dir(dfd, p, perm, follow, false, createcb);
    check_user(*fd);
    return fd;
  }

  int home();
  int home_jaim(bool create = false);
  int storage();
  const path &cwd()
  {
    if (cwd_.empty())
      cwd_ = canonical(std::filesystem::current_path());
    return cwd_;
  }

  const char *env_lookup(std::string_view var)
  {
    if (auto it = setenv_.find(var); it != setenv_.end())
      if (auto pos = it->second.find('='); pos != it->second.npos)
        return it->second.c_str() + pos + 1;
    return env_or_empty(var);
  }
  std::string expand(std::string_view in)
  {
    return parsing_config_file_
               ? var_expand(
                     in, [this](std::string_view v) { return env_lookup(v); })
               : std::string(in);
  }

  static bool name_ok(path p)
  {
    return p.is_relative() && components(p) == 1 && *p.c_str() != '.';
  }
};

template<> struct std::formatter<Config::Mode> : std::formatter<const char *> {
  using super = std::formatter<const char *>;
  auto format(Config::Mode m, auto &&ctx) const
  {
    using enum Config::Mode;
    switch (m) {
    case kStrict:
      return super::format("strict", ctx);
    case kBare:
      return super::format("bare", ctx);
    case kCasual:
      return super::format("casual", ctx);
    default:
      err<std::logic_error>("Config::Mode with bad value {}", int(m));
    }
  }
};
