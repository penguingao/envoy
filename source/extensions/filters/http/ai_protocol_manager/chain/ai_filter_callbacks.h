#pragma once

#include "envoy/event/dispatcher.h"
#include "envoy/stream_info/stream_info.h"

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

namespace Codec {
struct AiResponse;
struct AiResponseChunk;
struct AiItem;
} // namespace Codec

// Forward declare config (defined in filter_config.h) to avoid a circular dep.
class AiProtocolManagerConfig;

namespace Chain {

// AiEvent — lightweight tagged union for stats / access-log recording.
struct AiEvent {
  std::string name;
  std::string value;
};

// AiFilterCallbacks — the only way an AiFilter touches the world.
//
// Deliberately narrow: filters interact through the AiRequest / AiResponse
// model, not through raw HTTP buffer plumbing. The rule is:
//   "filters mutate AiRequest/AiResponse fields; the chain re-encodes."
//
// Headers ARE first-class (AiRequest::headers, AiResponse::headers) because
// HTTP headers need Envoy's native map for case-insensitivity and multi-value.
class AiFilterCallbacks {
public:
  virtual ~AiFilterCallbacks() = default;

  virtual Event::Dispatcher& dispatcher() = 0;
  virtual StreamInfo::StreamInfo& streamInfo() = 0;
  virtual const AiProtocolManagerConfig& config() = 0;

  // ── Continuation ─────────────────────────────────────────────────────────

  // Resume after StopIteration in any request-side phase.
  virtual void continueRequest() = 0;
  // Resume after StopIteration in any response-side phase.
  virtual void continueResponse() = 0;

  // ── Short-circuit — request side ─────────────────────────────────────────

  // Synthesize a direct reply without ever talking to upstream. Valid in any
  // request-side phase (Q1, Q2). Suppresses continueDecoding / dispatch.
  virtual void sendLocalReply(Codec::AiResponse&&) = 0;

  // ── Short-circuit — response side ────────────────────────────────────────

  // Cut an in-flight upstream response short and emit a synthetic tail.
  // Valid in any response-side phase (R1, R2, R3).
  virtual void endResponseEarly(Codec::AiResponse&&) = 0;

  // ── Per-item callbacks (valid only inside onRequestItem) ──────────────────

  // Drop the current item from the outgoing request — it will not appear in
  // the re-encoded body sent to the upstream.
  virtual void dropCurrentItem() = 0;

  // Insert a new item immediately after the current one; it flows through
  // subsequent filters and then into the re-encoded body.
  virtual void insertAfterItem(Codec::AiItem&&) = 0;

  // ── Per-chunk callbacks (valid only inside onResponseChunk) ───────────────

  // Drop the current chunk — it will not be forwarded downstream.
  virtual void dropCurrentChunk() = 0;

  // Inject a chunk after the current one; it flows through subsequent filters
  // and then downstream.
  virtual void insertAfterChunk(Codec::AiResponseChunk&&) = 0;

  // ── Observability ─────────────────────────────────────────────────────────

  virtual void recordEvent(AiEvent) = 0;
};

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
