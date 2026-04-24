#pragma once

#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/rest_transcoder_config.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Dispatch {

// AgenticDispatch — chain-forward tail for agentic (MCP / A2A) requests.
//
// Called at the end of the request-side chain phases (after Q1 and Q2) to
// re-encode the (possibly mutated) AiRequest with AgentPayload back into an
// HTTP request that downstream Envoy filters (e.g. the router) can process.
//
// Dispatch sequence (DESIGN.md §6.3, chain-forward mode):
//
//   1. RequestEncoder::encodeAgentBody(request)
//        → JSON-RPC body string reflecting any chain-filter mutations to
//          tool_name / resource_uri / prompt_name / arguments.
//
//   2. Mutate the downstream request header map in place via
//      callbacks.requestHeaders():
//        :method   ← request.http_method  (typically POST)
//        :path     ← request.path         (may be rewritten by routing filters)
//        content-type   ← "application/json"
//        content-length ← body byte count
//
//   3. Inject the re-encoded body buffer into the filter manager via
//      callbacks.addDecodedData().  The filter manager replays this buffer
//      to subsequent decoder filters and the router when continueDecoding()
//      resumes the decode chain.
//
//   4. callbacks.continueDecoding() — resumes the Envoy decode chain at the
//      filter after AiProtocolManagerFilter.
class AgenticDispatch : public Logger::Loggable<Logger::Id::filter> {
public:
  // Performs the full chain-forward dispatch for an agentic request.
  // `request` must have protocol == AgenticMcp or AgenticA2a.
  // `callbacks` is the StreamDecoderFilterCallbacks from the outer filter.
  // This method calls callbacks.continueDecoding() internally.
  static void dispatch(Codec::AiRequest& request,
                       Http::StreamDecoderFilterCallbacks& callbacks,
                       const McpRestTranscoderRouteConfig* transcoder = nullptr);
};

} // namespace Dispatch
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
