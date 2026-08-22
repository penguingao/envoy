#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// NormalizationFilter normalizes incoming provider request schemas (e.g. OpenAI Chat Completions)
// into a canonical representation for downstream governance and routing.
class NormalizationFilter : public AiFilter {
public:
  NormalizationFilter() = default;

  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
