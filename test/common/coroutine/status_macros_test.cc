#include <memory>

#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/status_macros.h"
#include "source/common/coroutine/task.h"

#include "test/common/coroutine/manual_executor.h"
#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Coroutine {
namespace {

using StatusHelpers::HasStatusMessage;
using StatusHelpers::IsOk;
using StatusHelpers::StatusCodeIs;

// Every coroutine here completes without suspending, so one drain() suffices.
template <typename T> T run(Task<T> task) {
  auto exec = std::make_shared<ManualExecutor>();
  std::optional<T> result;
  DetachedHandle handle =
      launch(std::move(task), exec, [&result](T value) { result = std::move(value); });
  exec->drain();
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

// Producers under test -------------------------------------------------------

absl::Status okStatus() { return absl::OkStatus(); }
absl::Status failedStatus() { return absl::InvalidArgumentError("bad input"); }

absl::StatusOr<int> valueOr(bool ok) {
  if (!ok) {
    return absl::NotFoundError("missing");
  }
  return 42;
}

// Awaited rather than called, covering the `co_await` spelling.
Task<absl::Status> awaitableStatus(bool ok) {
  co_return ok ? absl::OkStatus() : absl::InvalidArgumentError("bad input");
}

Task<absl::StatusOr<int>> awaitableValue(bool ok) {
  if (!ok) {
    co_return absl::NotFoundError("missing");
  }
  co_return 42;
}

// CO_RETURN_IF_ERROR ---------------------------------------------------------

TEST(CoReturnIfErrorTest, ContinuesPastOk) {
  bool reached_end = false;
  auto body = [&reached_end]() -> Task<absl::Status> {
    CO_RETURN_IF_ERROR(okStatus());
    reached_end = true;
    co_return absl::OkStatus();
  };

  EXPECT_THAT(run(body()), IsOk());
  EXPECT_TRUE(reached_end);
}

TEST(CoReturnIfErrorTest, ReturnsErrorAndSkipsRest) {
  bool reached_end = false;
  auto body = [&reached_end]() -> Task<absl::Status> {
    CO_RETURN_IF_ERROR(failedStatus());
    reached_end = true;
    co_return absl::OkStatus();
  };

  const absl::Status status = run(body());
  EXPECT_THAT(status, StatusCodeIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(status, HasStatusMessage("bad input"));
  EXPECT_FALSE(reached_end);
}

// One macro serves both Task return types because the status converts.
TEST(CoReturnIfErrorTest, ConvertsToStatusOrReturnType) {
  auto body = []() -> Task<absl::StatusOr<int>> {
    CO_RETURN_IF_ERROR(failedStatus());
    co_return 1;
  };

  EXPECT_THAT(run(body()).status(), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

TEST(CoReturnIfErrorTest, WorksOnAwaitedStatus) {
  auto body = []() -> Task<absl::Status> {
    CO_RETURN_IF_ERROR(co_await awaitableStatus(false));
    co_return absl::OkStatus();
  };

  EXPECT_THAT(run(body()), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

// A single statement, so it nests unbraced without swallowing the else. The
// took_else assertions would catch the else binding to the macro's own if.
TEST(CoReturnIfErrorTest, UsableInUnbracedIfElse) {
  bool took_else = false;
  auto body = [&took_else](bool cond) -> Task<absl::Status> {
    if (cond)
      CO_RETURN_IF_ERROR(failedStatus());
    else
      took_else = true;
    co_return absl::OkStatus();
  };

  EXPECT_THAT(run(body(true)), StatusCodeIs(absl::StatusCode::kInvalidArgument));
  EXPECT_FALSE(took_else);

  EXPECT_THAT(run(body(false)), IsOk());
  EXPECT_TRUE(took_else);
}

// Several uses in one scope must not collide.
TEST(CoReturnIfErrorTest, RepeatedUseInOneScopeEvaluatesOnce) {
  int calls = 0;
  auto count = [&calls]() {
    ++calls;
    return absl::OkStatus();
  };
  auto body = [&count]() -> Task<absl::Status> {
    CO_RETURN_IF_ERROR(count());
    CO_RETURN_IF_ERROR(count());
    co_return absl::OkStatus();
  };

  EXPECT_THAT(run(body()), IsOk());
  EXPECT_EQ(calls, 2);
}

// ASSIGN_OR_CO_RETURN --------------------------------------------------------

TEST(AssignOrCoReturnTest, DeclaresAndAssignsValue) {
  auto body = []() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto value, valueOr(true));
    co_return value + 1;
  };

  const absl::StatusOr<int> result = run(body());
  ASSERT_THAT(result, IsOk());
  EXPECT_EQ(*result, 43);
}

TEST(AssignOrCoReturnTest, AssignsToExistingVariable) {
  auto body = []() -> Task<absl::StatusOr<int>> {
    int value = 0;
    ASSIGN_OR_CO_RETURN(value, valueOr(true));
    co_return value;
  };

  const absl::StatusOr<int> result = run(body());
  ASSERT_THAT(result, IsOk());
  EXPECT_EQ(*result, 42);
}

TEST(AssignOrCoReturnTest, ReturnsStatusAndSkipsRest) {
  bool reached_end = false;
  auto body = [&reached_end]() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto value, valueOr(false));
    reached_end = true;
    co_return value;
  };

  const absl::StatusOr<int> result = run(body());
  EXPECT_THAT(result.status(), StatusCodeIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(result.status(), HasStatusMessage("missing"));
  EXPECT_FALSE(reached_end);
}

// A StatusOr-producing expression inside a Status-valued coroutine.
TEST(AssignOrCoReturnTest, ConvertsToStatusReturnType) {
  auto body = []() -> Task<absl::Status> {
    ASSIGN_OR_CO_RETURN(auto value, valueOr(false));
    EXPECT_EQ(value, 0); // Unreached; keeps `value` used.
    co_return absl::OkStatus();
  };

  EXPECT_THAT(run(body()), StatusCodeIs(absl::StatusCode::kNotFound));
}

TEST(AssignOrCoReturnTest, WorksOnAwaitedValue) {
  auto body = []() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto value, co_await awaitableValue(true));
    co_return value;
  };

  const absl::StatusOr<int> result = run(body());
  ASSERT_THAT(result, IsOk());
  EXPECT_EQ(*result, 42);
}

// Distinct temporaries per use.
TEST(AssignOrCoReturnTest, RepeatedUseInOneScope) {
  auto body = []() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto first, valueOr(true));
    ASSIGN_OR_CO_RETURN(auto second, valueOr(true));
    co_return first + second;
  };

  const absl::StatusOr<int> result = run(body());
  ASSERT_THAT(result, IsOk());
  EXPECT_EQ(*result, 84);
}

TEST(AssignOrCoReturnTest, EvaluatesExpressionOnce) {
  int calls = 0;
  auto count = [&calls]() -> absl::StatusOr<int> {
    ++calls;
    return 1;
  };
  auto body = [&count]() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto value, count());
    co_return value;
  };

  EXPECT_THAT(run(body()), IsOk());
  EXPECT_EQ(calls, 1);
}

// The value is moved out, so a move-only payload works.
TEST(AssignOrCoReturnTest, MovesMoveOnlyValue) {
  auto produce = []() -> absl::StatusOr<std::unique_ptr<int>> { return std::make_unique<int>(7); };
  auto body = [&produce]() -> Task<absl::StatusOr<int>> {
    ASSIGN_OR_CO_RETURN(auto value, produce());
    co_return *value;
  };

  const absl::StatusOr<int> result = run(body());
  ASSERT_THAT(result, IsOk());
  EXPECT_EQ(*result, 7);
}

} // namespace
} // namespace Coroutine
} // namespace Envoy
