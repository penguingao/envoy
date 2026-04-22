#pragma once

#include <any>
#include <cstdint>
#include <string>
#include <variant>

#include "envoy/http/header_map.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/agent_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_payload.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// DESIGN.md §4.6 — shared envelope + variant summary for responses.
//
// Symmetric to AiRequest: cross-cutting filters see scalar envelope fields
// and a protocol-neutral summary variant, while the per-event response
// content flows as a stream of AiResponseChunk (Phase 4c). Buffering the
// whole response before running the chain would defeat the point of
// streaming-first design — the summary is for "headers arrived" /
// "stream ended" scalars.
class AiResponse {
public:
  AiResponse() = default;

  // --- HTTP-level. Populated at onResponseStart time. ---
  uint32_t http_status{0};
  // Non-owning view of upstream response headers (owned by the dispatch
  // filter's AsyncStream). Filters may read / mutate; ResponseEncoder
  // re-emits downstream from this map.
  Http::ResponseHeaderMap* headers{nullptr};

  // --- Correlates with the AiRequest that produced this response. ---
  std::string jsonrpc_id;
  ProtocolKind protocol{ProtocolKind::Unknown};

  // --- Protocol-specific scalar summary. ---
  std::variant<std::monostate, InferenceResponseSummary, AgentResponseSummary> summary;

  // --- Streaming intent carried from the request. ---
  bool streaming{false};

  // --- Payload offload: not owned; outer filter owns the store. ---
  PayloadStore* payload_store{nullptr};

  // --- Filter-to-filter scratch within this response. ---
  AiScratch scratch;

  // --- Typed accessors. Return nullptr on wrong variant. ---
  InferenceResponseSummary* asInference();
  const InferenceResponseSummary* asInference() const;
  AgentResponseSummary* asAgent();
  const AgentResponseSummary* asAgent() const;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
