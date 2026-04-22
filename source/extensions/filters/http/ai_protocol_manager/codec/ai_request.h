#pragma once

#include <any>
#include <string>
#include <variant>

#include "envoy/http/header_map.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/agent_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_payload.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// DESIGN.md §4.1 — shared envelope + variant payload.

enum class ProtocolKind { Unknown, Inference, AgentA2a, AgentMcp };

// Per-filter scratch shared across sub-chain filters; not serialized, not
// cross-request.
using AiScratch = absl::flat_hash_map<std::string, std::any>;

class AiRequest {
public:
  AiRequest() = default;

  // --- HTTP-level identity. Populated at decodeHeaders time by the outer
  //     filter / RequestDecoder; the dispatch filter rebuilds an equivalent
  //     outbound request from these per the §4.3 round-trip invariant. ---
  std::string http_method;  // "GET", "POST", "DELETE", "PATCH", ...
  std::string path;         // e.g. "/v1/responses/resp_abc123"
  // Parsed path parameters (e.g. {"response_id": "resp_abc123"}), populated
  // by the classifier from a path pattern. Empty when the path carries no
  // template variables.
  absl::flat_hash_map<std::string, std::string> path_params;
  // Raw query string key/values — populated by the classifier / decoder.
  absl::flat_hash_map<std::string, std::string> query_params;
  // Non-owning view of the downstream request headers (owned by the outer
  // filter's stream). Filters may read and mutate in place; the encoder uses
  // this when building the upstream request. Native Envoy map so we keep
  // case-insensitivity, inline slots, and multi-value support.
  Http::RequestHeaderMap* headers{nullptr};

  // --- JSON-RPC identity (populated only for JSON-RPC bodies; empty for
  //     REST-ish or bodiless requests). ---
  std::string jsonrpc_id;  // empty ⇒ notification / non-JSON-RPC
  std::string rpc_method;  // raw JSON-RPC "method" token when present

  // --- Protocol discriminator + variant payload ---
  ProtocolKind protocol{ProtocolKind::Unknown};
  std::variant<std::monostate, InferencePayload, AgentPayload> payload;

  // --- Neutral scalars that arrived with the request (tenant, user id,
  //     request-id, routing hints). Cross-cutting filters read from here. ---
  absl::flat_hash_map<std::string, std::string> attributes;

  // --- Streaming intent (OpenAI stream:true, A2A/MCP SSE subscribe,
  //     Responses GET with stream=true reattach). ---
  bool streaming{false};

  // --- Payload offload: not owned; outer filter owns the store. ---
  PayloadStore* payload_store{nullptr};

  // --- Filter-to-filter scratch within this request. ---
  AiScratch scratch;

  // --- Typed accessors. Return nullptr on wrong variant. ---
  InferencePayload* asInference();
  const InferencePayload* asInference() const;
  AgentPayload* asAgent();
  const AgentPayload* asAgent() const;
};

// AiResponse moved to codec/ai_response.h as of Phase 4b — it now carries
// the same envelope + variant-summary shape as AiRequest per DESIGN §4.6.

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
