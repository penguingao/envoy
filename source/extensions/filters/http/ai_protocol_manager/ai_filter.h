#pragma once

#include <memory>
#include <vector>

#include "source/common/coroutine/task.h"

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class AiRequest;
class FieldStreamingSession;

// Callable awaitable that delivers the `AiRequest` to a filter.
using AiRequestGetter =
    absl::AnyInvocable<Coroutine::Task<absl::StatusOr<std::unique_ptr<AiRequest>>>() &&>;

// Callable awaitable that forwards the `AiRequest` to the next filter in the chain.
using AiRequestForwarder =
    absl::AnyInvocable<Coroutine::Task<absl::StatusOr<std::unique_ptr<FieldStreamingSession>>>(
        std::unique_ptr<AiRequest>) &&>;

// Abstract interface implemented by `AiFilter` instances.
class AiFilter {
public:
  virtual ~AiFilter() = default;

  // Invoked when an AI request arrives.
  // Returns absl::OkStatus() on normal completion, or an error status on failure.
  virtual Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                               AiRequestForwarder req_forwarder) = 0;
};

using AiFilterPtr = std::unique_ptr<AiFilter>;

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
