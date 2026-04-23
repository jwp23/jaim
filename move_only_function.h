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
 *
 * Modified 2026 by Claude Opus 4.7 (supervised by Greg Slepak): match
 *   forward declaration tag (class) to the primary template to silence
 *   -Wmismatched-tags.
 */

#pragma once

#include <functional>

#if __cpp_lib_move_only_function >= 202110L

using std::move_only_function;

#else // std::move_only_function unimplemented

#include <memory>

template<typename F> class move_only_function;

template<typename R, typename... Args, bool NE>
class move_only_function<R(Args...) noexcept(NE)> {
public:
  struct Invoker {
    virtual ~Invoker() = default;
    virtual R operator()(Args... args) noexcept(NE) = 0;
  };
  template<typename F> struct Impl : Invoker {
    F f_;
    // Impl(F f) : f_(std::move(f)) {}
    template<typename RF> Impl(RF &&f) : f_(std::forward<RF>(f)) {}
    R operator()(Args... args) noexcept(NE) override
    {
      return std::invoke(f_, std::forward<Args>(args)...);
    }
  };
  template<typename F> Impl(F &&) -> Impl<std::decay_t<F>>;

  std::unique_ptr<Invoker> inv_;

public:
  move_only_function() noexcept = default;
  move_only_function(std::nullptr_t) noexcept {};
  move_only_function(move_only_function &&) noexcept = default;

  template<std::invocable<Args...> F>
  move_only_function(F f) : inv_(new Impl{std::move(f)})
  {}

  move_only_function &operator=(move_only_function &&) noexcept = default;

  R operator()(Args ...args) noexcept(NE)
  {
    return (*inv_)(std::forward<Args>(args)...);
  }

  explicit operator bool() noexcept { return inv_; }
  bool operator==(std::nullptr_t) noexcept { return !inv_; }
};

#endif // std::move_only_function unimplemented
