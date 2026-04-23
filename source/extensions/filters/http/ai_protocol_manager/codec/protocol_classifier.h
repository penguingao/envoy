#pragma once

#include <string>
#include <variant>

#include "envoy/http/header_map.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

struct ClassifyInput {
  absl::string_view http_method;
  absl::string_view path;
  const Http::RequestHeaderMap& headers;
  // JSON-RPC "method" token — empty for REST / bodiless on first call;
  // populated on re-classify once the body token has been seen.
  absl::string_view rpc_method;
};

struct ClassifyResult {
  ProtocolKind protocol{ProtocolKind::Unknown};
  // Populated when known from path/method/rpc_method; monostate otherwise.
  std::variant<std::monostate, InferenceInvocation, AgentInvocation> invocation;
  // Path parameters extracted by pattern matching (e.g. {"id": "resp_abc"}).
  absl::flat_hash_map<std::string, std::string> path_params;
};

// Classifies an HTTP request into a ProtocolKind and (when determinable)
// a specific invocation. Called twice for agentic requests: once on headers
// (rpc_method empty) and again when the JSON-RPC "method" token is seen in
// the body (rpc_method non-empty). The second call refines the invocation.
ClassifyResult classify(const ClassifyInput& input);

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
