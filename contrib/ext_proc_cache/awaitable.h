#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// A value-returning coroutine type. Callers co_await an Awaitable<T> to get T.
// Supports both synchronous completion (await_ready() == true when the value is
// already available) and asynchronous completion (suspend caller until value is set).
template <typename T> class Awaitable {
public:
  struct promise_type {
    std::optional<T> value;
    std::exception_ptr exception;
    std::coroutine_handle<> caller;

    Awaitable get_return_object() {
      return Awaitable{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() { return {}; }

    auto final_suspend() noexcept {
      struct Resumer {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          auto caller = h.promise().caller;
          return caller ? caller : std::noop_coroutine();
        }
        void await_resume() noexcept {}
      };
      return Resumer{};
    }

    void return_value(T val) { value.emplace(std::move(val)); }

    void unhandled_exception() { exception = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  explicit Awaitable(handle_type h) : handle_(h) {}

  // Move-only.
  Awaitable(Awaitable&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Awaitable& operator=(Awaitable&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  Awaitable(const Awaitable&) = delete;
  Awaitable& operator=(const Awaitable&) = delete;

  ~Awaitable() {
    if (handle_) {
      handle_.destroy();
    }
  }

  bool await_ready() const { return handle_.done(); }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
    handle_.promise().caller = caller;
    return handle_;
  }

  T await_resume() {
    auto& promise = handle_.promise();
    if (promise.exception) {
      std::rethrow_exception(promise.exception);
    }
    return std::move(*promise.value);
  }

  // Drive the coroutine to completion synchronously and return the result.
  // Only valid for coroutines that complete without suspending (e.g.
  // InMemoryCacheStore operations that use co_return immediately).
  T syncGet() {
    if (!handle_.done()) {
      handle_.resume();
    }
    return await_resume();
  }

  // Factory for creating an Awaitable that is immediately ready with a value.
  static Awaitable ready(T val) {
    co_return std::move(val);
  }

private:
  handle_type handle_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
