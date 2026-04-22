#pragma once

#include "envoy/event/dispatcher.h"
#include "envoy/stream_info/stream_info.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_chunk.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

class AiItem; // chain/ai_filter_chain.h

// Forward-declared event type for recordEvent(). Kept opaque here; concrete
// struct defined alongside the chain implementation. Avoids a cyclic include.
struct AiEvent;

// DESIGN.md §5.2 — narrow interface through which an AiFilter interacts with
// the world. Filters use the AiRequest / AiResponse model (which exposes
// real Http::Request/ResponseHeaderMap pointers); they do not get raw
// Buffer::Instance, route config, or the cluster manager. The rule is "no
// side-channel HTTP plumbing," not "no Envoy HTTP types."
class AiFilterCallbacks {
public:
  virtual ~AiFilterCallbacks() = default;

  virtual Event::Dispatcher& dispatcher() = 0;
  virtual StreamInfo::StreamInfo& streamInfo() = 0;

  // Resume after StopIteration. Valid at whatever granularity the pause
  // happened (any request-side or response-side phase).
  virtual void continueRequest() = 0;
  virtual void continueResponse() = 0;

  // Short-circuit BEFORE dispatch: never talks to upstream. Synthesizes a
  // direct reply (e.g. guardrail denial on the request side). Valid in any
  // request-side phase.
  virtual void sendLocalReply(Codec::AiResponse&& response) = 0;

  // Short-circuit DURING/AFTER dispatch: upstream is already engaged; cut
  // the in-flight response short and emit a synthetic tail downstream.
  // Valid in any response-side phase. Per ARCHITECTURE §2 retry contract,
  // this is terminal — no re-dispatch after bytes have flowed.
  virtual void endResponseEarly(Codec::AiResponse&& response) = 0;

  // Per-item callbacks (valid only inside onRequestItem).
  virtual void dropCurrentItem() = 0;
  virtual void insertAfter(AiItem&& item) = 0;

  // Per-chunk callbacks (valid only inside onResponseChunk).
  // Don't forward this chunk downstream.
  virtual void dropCurrentChunk() = 0;
  // Inject a chunk after the current one. Flows through subsequent filters,
  // then downstream. Useful for splicing a synthetic system message or a
  // guardrail notice into the stream.
  virtual void insertAfter(Codec::AiResponseChunk&& chunk) = 0;

  // Observability entry point.
  virtual void recordEvent(const AiEvent& event) = 0;
};

// Pure abort-on-call callbacks impl. Suitable only as a placeholder for an
// empty chain (where no filter actually calls back). Using it with a
// non-empty chain is a programming error.
class UnreachableCallbacks : public AiFilterCallbacks {
public:
  Event::Dispatcher& dispatcher() override;
  StreamInfo::StreamInfo& streamInfo() override;
  void continueRequest() override;
  void continueResponse() override;
  void sendLocalReply(Codec::AiResponse&&) override;
  void endResponseEarly(Codec::AiResponse&&) override;
  void dropCurrentItem() override;
  void insertAfter(AiItem&&) override;
  void dropCurrentChunk() override;
  void insertAfter(Codec::AiResponseChunk&&) override;
  void recordEvent(const AiEvent&) override;
};

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
