#pragma once

#include <string>
#include <variant>
#include <vector>

#include "envoy/http/header_map.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/agent_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_payload.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// DESIGN.md §4.4 — decides Inference vs Agent (and which agent dialect) from
// HTTP verb + path + headers + JSON-RPC method token. Extracts path_params for
// URL-template routes (Responses resource ops etc.) so the mapper does not
// have to re-parse the path.
//
// V0 implementation is path-prefix based. Path-template extraction and
// invocation disambiguation land incrementally with the Responses resource
// ops in a later phase; the signature is shaped now so callers do not have
// to churn when that arrives.

struct ClassifierPrefixes {
  std::vector<std::string> inference_prefixes;
  std::vector<std::string> agent_prefixes;
};

struct ClassifyInput {
  absl::string_view http_method;
  absl::string_view path;
  const Http::RequestHeaderMap& headers;
  // JSON-RPC "method" token (empty for REST / bodiless).
  absl::string_view rpc_method;
  const ClassifierPrefixes& prefixes;
};

struct ClassifyResult {
  ProtocolKind protocol{ProtocolKind::Unknown};
  // Populated when known from headers/path alone (before body parsing).
  std::variant<std::monostate, InferenceInvocation, AgentInvocation> invocation;
  // Extracted path params (e.g. response_id), ready to copy into
  // AiRequest::path_params. Empty when the matched pattern has no variables.
  absl::flat_hash_map<std::string, std::string> path_params;
};

ClassifyResult classify(const ClassifyInput& input);

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
