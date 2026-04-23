#include "source/extensions/filters/http/ai_protocol_manager/codec/request_encoder.h"

#include "source/common/common/assert.h"
#include "source/common/json/json_streamer.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// ── encodeAgentBody ───────────────────────────────────────────────────────────
//
// Produces the JSON-RPC wire body for an agentic request:
//
//   {"jsonrpc":"2.0","id":"<id>","method":"<method>","params":<params>}
//
// Three categories of invocations:
//
//  A. Fully-structured params (ToolsCall, Resources*, PromptsGet)
//     → params object rebuilt from AgentPayload fields so chain-filter
//       mutations to tool_name / resource_uri / prompt_name / arguments
//       are visible in the outgoing request.
//
//  B. All other invocations (Initialize, Ping, list ops, A2A, …)
//     → params_raw is inserted verbatim; this preserves all fields that the
//       decoder did not extract into structured form (e.g. protocolVersion,
//       clientInfo, cursor, message.parts).  Mutations to AgentPayload fields
//       not in category A are NOT reflected in the body for these invocations.
//
// The "id" field is omitted when jsonrpc_id is empty (JSON-RPC notification).

std::string RequestEncoder::encodeAgentBody(const AiRequest& request) {
  const AgentPayload* payload = request.as_agent();
  ASSERT(payload != nullptr);
  if (payload == nullptr) {
    return "{}";
  }

  std::string out;
  Json::StringOutput so(out);
  Json::StringStreamer streamer(so);

  auto root = streamer.makeRootMap();

  root->addKey("jsonrpc");
  root->addString("2.0");

  // id is omitted for notifications (empty jsonrpc_id).
  if (!request.jsonrpc_id.empty()) {
    root->addKey("id");
    root->addString(request.jsonrpc_id);
  }

  root->addKey("method");
  root->addString(request.rpc_method);

  root->addKey("params");

  switch (payload->invocation) {

  // ── Category A: fully-structured params ──────────────────────────────────

  case AgentInvocation::ToolsCall: {
    // MCP tools/call → {"name":"<tool>","arguments":<args>}
    auto params = root->addMap();
    params->addKey("name");
    params->addString(payload->tool_name);
    if (!payload->arguments.empty()) {
      params->addKey("arguments");
      params->addRawJson(payload->arguments.toString());
    }
    break;
  }

  case AgentInvocation::ResourcesRead:
  case AgentInvocation::ResourcesSubscribe:
  case AgentInvocation::ResourcesUnsubscribe: {
    // MCP resources/{read,subscribe,unsubscribe} → {"uri":"<uri>"}
    auto params = root->addMap();
    params->addKey("uri");
    params->addString(payload->resource_uri);
    break;
  }

  case AgentInvocation::PromptsGet: {
    // MCP prompts/get → {"name":"<prompt>","arguments":<args>}
    auto params = root->addMap();
    params->addKey("name");
    params->addString(payload->prompt_name);
    if (!payload->arguments.empty()) {
      params->addKey("arguments");
      params->addRawJson(payload->arguments.toString());
    }
    break;
  }

  // ── Category B: pass-through from params_raw ─────────────────────────────

  default:
    // For Initialize, Ping, list ops, CompletionComplete, LoggingSetLevel,
    // SamplingCreateMessage, and all A2A operations, use the raw params JSON
    // captured by the decoder.  Falls back to an empty object when params_raw
    // is absent (e.g. notification with no params field).
    if (payload->params_raw.empty()) {
      root->addRawJson("{}");
    } else {
      root->addRawJson(payload->params_raw.toString());
    }
    break;
  }

  root.reset(); // emits closing "}"
  return out;
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
