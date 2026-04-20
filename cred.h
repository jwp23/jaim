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
 * Modified 2026 by Joseph Presley: add kJaimSystemUser, get_jaim_user()
 *   lookup, and setup_jaim_user/remove_jaim_user declarations backing
 *   --setup-user and --remove-user (ja-du9).
 * Modified 2026 by Joseph Presley: add Credentials::enter_permanently
 *   for strict-mode UID separation: drops root to this Credentials via
 *   setgroups/setgid/setuid in the forked child before sandbox_init
 *   (ja-txx).
 */

#pragma once

#include "err.h"

#include <format>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

// Something that can contain a user or group id
using ugid_t = std::common_type_t<uid_t, gid_t>;

// Wrapper around getpw{nam,uid}_r and getgr{nam,gid}_id
template<typename Ent, auto IdFn, auto NamFn> struct DbEnt {
  Ent *p_{};
  Ent ent_;
  std::vector<char> buf_;

  DbEnt() noexcept = default;
  DbEnt(DbEnt &&other) : ent_(other.ent_), buf_(exchange(other.buf_, {}))
  {
    p_ = other.p_ ? &ent_ : nullptr;
    other.p_ = nullptr;
  }
  DbEnt &operator=(DbEnt &&other) noexcept
  {
    p_ = other.p_ ? &ent_ : nullptr;
    other.p_ = nullptr;
    ent_ = other.ent_;
    buf_ = std::exchange(other.buf_, {});
    return *this;
  }

  explicit operator bool() const { return p_; }
  const Ent *operator->() const { return p_; }
  Ent *get() const { return p_; }

  static DbEnt get_id(ugid_t n) { return find(IdFn, n); }
  static DbEnt get_nam(const char *n) { return find(NamFn, n); }

  static DbEnt find(auto fn, auto key)
  {
    DbEnt ret;
    ret.buf_.resize(std::max(128uz, ret.buf_.capacity()));
    for (;;) {
      int r = fn(key, &ret.ent_, ret.buf_.data(), ret.buf_.size(), &ret.p_);
      if (!r)
        return ret ? std::move(ret) : DbEnt{};
      else if (r == ERANGE)
        ret.buf_.resize(2 * ret.buf_.size());
      else if (r == ENOENT)
        return DbEnt{};
      else
        errno = r, syserr("DbEnt<{}>::find", typeid(Ent).name());
    }
  }
};
using PwEnt = DbEnt<passwd, getpwuid_r, getpwnam_r>;
using GrEnt = DbEnt<group, getgrgid_r, getgrnam_r>;

// Dedicated unprivileged system user that strict mode drops privileges
// into before launching the sandboxed child.  The name is kept here
// rather than hardcoded at each callsite so the setup/remove helpers
// and the future strict-mode UID-separation code (ja-txx) agree on
// what to create, remove, and look up.
inline constexpr char kJaimSystemUser[] = "_jaim";

struct Credentials {
  uid_t uid_ = -1;
  gid_t gid_ = -1;
  std::vector<gid_t> groups_;

  void make_effective() const;
  void make_real() const;
  // Permanently drop from root to this Credentials' uid/gid/groups via
  // setgroups, setgid, setuid (in that order).  Requires euid == 0 on
  // entry and *this to be populated; throws via err() otherwise.  On
  // macOS setuid() from root replaces real, effective, *and* saved
  // uid, so this is a one-way door — no code after the call can
  // regain root.  Used by Config::exec()'s child in strict mode,
  // before sandbox_init, so the sandboxed process starts at an
  // unprivileged UID that macOS TCC has no grants for (camera, mic,
  // contacts, calendar, and the other privacy-gated resources are
  // all blocked for _jaim regardless of what the Seatbelt profile
  // allows, because TCC is keyed on the process identity rather
  // than on the profile).
  void enter_permanently() const;
  std::string show() const;
  explicit operator bool() const noexcept { return uid_ != -1; }

  static Credentials get_user(const struct passwd *pw);
  static Credentials get_user(const PwEnt &e) { return get_user(e.get()); }
  // Look up the _jaim system user.  Returns a falsy Credentials (uid_
  // == -1) if the user is not in the password database — callers that
  // require the user present must check and report with a pointer to
  // --setup-user.
  static Credentials get_jaim_user();
  static Credentials get_effective()
  {
    return Credentials{
        .uid_ = geteuid(),
        .gid_ = getegid(),
        .groups_ = getgroups(),
    };
  }
  static Credentials get_real()
  {
    return Credentials{
        .uid_ = getuid(),
        .gid_ = getgid(),
        .groups_ = getgroups(),
    };
  }

  static std::vector<gid_t> getgroups();

  friend bool operator==(const Credentials &,
                         const Credentials &) noexcept = default;
};

// Create the _jaim system user (and its primary group) via dscl.
// Requires euid == 0; throws otherwise.  Idempotent: a successful
// return leaves the user present, whether this call created it or
// found it already there.  The UID and GID are picked from the
// macOS system range (< 500) by scanning for unused values.  Home
// directory is /var/empty, shell is /usr/bin/false, so the account
// cannot log in interactively and has no writable home.
void setup_jaim_user();

// Remove the _jaim system user and its primary group via dscl.
// Requires euid == 0; throws otherwise.  Idempotent: succeeds
// whether the user/group was present or not.  The records are the
// only persistent state jaim's setup creates on the host, so this
// is the full uninstall for that piece.
void remove_jaim_user();

template<>
struct std::formatter<Credentials> : std::formatter<std::string_view> {
  using super = std::formatter<std::string_view>;
  auto format(const Credentials &creds, auto &ctx) const
  {
    return super::format(creds.show(), ctx);
  }
};
