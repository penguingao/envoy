#include "source/extensions/filters/http/ai_protocol_manager/normalization_filter.h"

#include <memory>
#include <utility>

#include "source/common/coroutine/status_macros.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_streaming_session.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

Coroutine::Task<absl::Status> NormalizationFilter::decode(AiRequestGetter req_getter,
                                                          AiRequestForwarder req_forwarder) {
  ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
  if (req == nullptr) {
    co_return absl::InvalidArgumentError("AiRequest is null in NormalizationFilter");
  }

  // Normalization logic: ensure top-level messages exists
  auto& json = req->doc().json();
  if (json.is_object() && !json.contains("messages")) {
    json["messages"] = nlohmann::json::array();
  }

  ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                      co_await std::move(req_forwarder)(std::move(req)));

  // Streaming session can be used if normalization filter needs to stream slices
  if (session != nullptr) {
    ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
    while (buf_opt.has_value()) {
      session->publish(*buf_opt);
      ASSIGN_OR_CO_RETURN(buf_opt, co_await session->fetch());
    }
  }

  co_return absl::OkStatus();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
