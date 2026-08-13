#pragma once

#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

/**
 * Early-return macros for coroutine bodies.
 *
 *   Task<absl::StatusOr<Reply>> call() {
 *     ASSIGN_OR_CO_RETURN(auto conn, co_await connect());
 *     CO_RETURN_IF_ERROR(co_await conn->handshake());
 *     co_return co_await conn->read();
 *   }
 *
 * Both Task return types (absl::Status, absl::StatusOr<U>) are constructible
 * from absl::Status, so these serve either. CO_RETURN_IF_ERROR is a single
 * statement. ASSIGN_OR_CO_RETURN cannot be, since it declares a variable the
 * following code uses: it needs a braced block, and its `lhs` must not contain
 * an unparenthesized comma.
 */

namespace Envoy {
namespace Coroutine {

// Two levels of indirection so __COUNTER__ expands before pasting, giving each
// use its own temporary.
#define ENVOY_CORO_CONCAT_INNER_(a, b) a##b
#define ENVOY_CORO_CONCAT_(a, b) ENVOY_CORO_CONCAT_INNER_(a, b)

// Evaluates `expr` once; co_returns it if it is not OK.
#define CO_RETURN_IF_ERROR(expr)                                                                   \
  do {                                                                                             \
    if (absl::Status co_temp_status = (expr); !co_temp_status.ok()) {                              \
      co_return co_temp_status;                                                                    \
    }                                                                                              \
  } while (0)

// Evaluates `rexpr` once; co_returns its status on failure, otherwise moves the
// value into `lhs`, which may be a declaration:
//
//   ASSIGN_OR_CO_RETURN(auto value, co_await produce());
//   ASSIGN_OR_CO_RETURN(existing_value, co_await produce());
#define ASSIGN_OR_CO_RETURN(lhs, rexpr)                                                            \
  ENVOY_ASSIGN_OR_CO_RETURN_IMPL_(ENVOY_CORO_CONCAT_(co_temp_statusor_, __COUNTER__), lhs, rexpr)

#define ENVOY_ASSIGN_OR_CO_RETURN_IMPL_(temp, lhs, rexpr)                                          \
  auto temp = (rexpr);                                                                             \
  if (!temp.ok()) {                                                                                \
    co_return std::move(temp).status();                                                            \
  }                                                                                                \
  lhs = std::move(temp).value()

} // namespace Coroutine
} // namespace Envoy
