
#include "cred.h"

#include <grp.h>

void
Credentials::make_effective() const
{
  // On macOS, jaim doesn't run as setuid root, so credential
  // switching is limited. Only attempt if running as root.
  if (geteuid() != 0)
    return;
  if (groups_ != getgroups() && setgroups(groups_.size(), groups_.data()))
    syserr("setgroups");
  if (gid_ != getegid() && setegid(gid_))
    syserr("setegid");
  if (seteuid(uid_))
    syserr("seteuid");
}

void
Credentials::make_real() const
{
  if (geteuid() != 0)
    return;
  if (groups_ != getgroups() && setgroups(groups_.size(), groups_.data()))
    syserr("setgroups");
  if (gid_ != getgid() && setgid(gid_))
    syserr("setgid");
  if (setuid(uid_))
    syserr("setuid");
}

std::string
Credentials::show() const
{
  auto ret = std::format("uid={} gid={}", uid_, gid_);
  if (!groups_.empty()) {
    ret += " groups=";
    bool first = true;
    for (gid_t g : groups_) {
      if (first)
        first = false;
      else
        ret += ',';
      ret += std::to_string(g);
    }
  }
  return ret;
}

Credentials
Credentials::get_user(const struct passwd *pw)
{
  Credentials ret{.uid_ = pw->pw_uid, .gid_ = pw->pw_gid};
  // macOS getgrouplist uses int* for groups, not gid_t*
  int n = 0;
  if (getgrouplist(pw->pw_name, static_cast<int>(pw->pw_gid), nullptr, &n) != -1)
    return ret;
  std::vector<int> igroups(n);
  if (getgrouplist(pw->pw_name, static_cast<int>(pw->pw_gid),
                   igroups.data(), &n) < 0)
    err("getgrouplist({}) failed", pw->pw_name);
  igroups.resize(n);
  ret.groups_.assign(igroups.begin(), igroups.end());
  return ret;
}

std::vector<gid_t>
Credentials::getgroups()
{
  for (int i = 0; i < 4; ++i) {
    int n = ::getgroups(0, nullptr);
    if (n < 0)
      syserr("getgroups");
    auto ret = std::vector<gid_t>(size_t(n));
    n = ::getgroups(ret.size(), ret.data());
    if (n >= 0) {
      if (size_t(n) > ret.size())
        err("getgroups: expected {} groups but got {}", ret.size(), n);
      ret.resize(n);
      return ret;
    }
  }
  err("getgroups: consistently unable to get actual groups");
}
