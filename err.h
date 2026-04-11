// -*-C++-*-
/*
 * jaim - Sandboxing tool for AI CLI access
 * Copyright (C) 2026 David Mazieres
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

#pragma once

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

extern std::filesystem::path prog;

// Format error message and throw an exception that captures errno
template<typename... Args>
[[noreturn]] inline void
syserr(std::format_string<Args...> fmt, Args &&...args)
{
  throw std::system_error(
      errno, std::system_category(),
      std::vformat(fmt.get(), std::make_format_args(args...)));
}

// Format error message and throw exception
template<typename E = std::runtime_error, typename... Args>
[[noreturn]] inline void
err(std::format_string<Args...> fmt, Args &&...args)
{
  throw E(std::vformat(fmt.get(), std::make_format_args(args...)));
}

template<typename... Args>
inline void
warn(std::format_string<Args...> fmt, Args &&...args)
{
  std::string msg = prog.filename();
  msg += ": ";
  msg += std::vformat(fmt.get(), std::make_format_args(args...));
  msg += '\n';
  write(2, msg.c_str(), msg.size());
}
