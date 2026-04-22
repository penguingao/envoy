#pragma once

#include <memory>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_chunk.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// DESIGN.md §5.1 — ergonomic, phased filter interface presented to operators
// writing sub-chain filters.

class AiFilterCallbacks; // fwd; chain/ai_filter_callbacks.h

enum class AiFilterStatus {
  Continue,       // advance to next filter (same phase)
  StopIteration,  // pause; resume via cb.continueRequest() / continueResponse()
};

enum class AiItemKind { Message, Tool, Attachment };

// Bitset: which request-item kinds this filter wants onRequestItem callbacks
// for. Unioned across filters at chain-build time so the runtime can skip
// materializing kinds nobody cares about.
struct AiItemKindSet {
  bool messages{false};
  bool tools{false};
  bool attachments{false};

  static AiItemKindSet all() { return AiItemKindSet{true, true, true}; }
  static AiItemKindSet none() { return AiItemKindSet{false, false, false}; }

  bool any() const { return messages || tools || attachments; }
  AiItemKindSet unionWith(const AiItemKindSet& other) const {
    return AiItemKindSet{messages || other.messages, tools || other.tools,
                         attachments || other.attachments};
  }
};

// Bitset: which response-chunk kinds this filter wants onResponseChunk
// callbacks for. Same phase-skip pattern as AiItemKindSet — chunks of
// unclaimed kinds pass through to downstream without materialization.
struct AiChunkKindSet {
  bool started{false};
  bool item_added{false};
  bool content_delta{false};
  bool reasoning_delta{false};
  bool tool_call_delta{false};
  bool item_done{false};
  bool completed{false};
  bool error_event{false};
  bool final_body{false};
  bool raw{false};

  static AiChunkKindSet all() {
    return AiChunkKindSet{true, true, true, true, true, true, true, true, true, true};
  }
  static AiChunkKindSet none() { return AiChunkKindSet{}; }

  bool contains(Codec::AiChunkKind k) const {
    switch (k) {
    case Codec::AiChunkKind::Started:
      return started;
    case Codec::AiChunkKind::ItemAdded:
      return item_added;
    case Codec::AiChunkKind::ContentDelta:
      return content_delta;
    case Codec::AiChunkKind::ReasoningDelta:
      return reasoning_delta;
    case Codec::AiChunkKind::ToolCallDelta:
      return tool_call_delta;
    case Codec::AiChunkKind::ItemDone:
      return item_done;
    case Codec::AiChunkKind::Completed:
      return completed;
    case Codec::AiChunkKind::ErrorEvent:
      return error_event;
    case Codec::AiChunkKind::Final:
      return final_body;
    case Codec::AiChunkKind::Raw:
      return raw;
    }
    return false;
  }

  AiChunkKindSet unionWith(const AiChunkKindSet& o) const {
    return AiChunkKindSet{started || o.started,
                          item_added || o.item_added,
                          content_delta || o.content_delta,
                          reasoning_delta || o.reasoning_delta,
                          tool_call_delta || o.tool_call_delta,
                          item_done || o.item_done,
                          completed || o.completed,
                          error_event || o.error_event,
                          final_body || o.final_body,
                          raw || o.raw};
  }
};

// Runtime-side materialized item view. Filters see this during the per-item
// phase; they mutate it in place (and set dirty()) to trigger re-store. See
// DESIGN.md §4.5. Concrete struct defined in chain/ai_filter_chain.h so the
// interface here only forward-declares it.
class AiItem;

class AiFilter {
public:
  virtual ~AiFilter() = default;

  // ======================== Request side ========================

  // Q1: scalars only. Always invoked. Does not trigger payload
  // materialization. Most cross-cutting filters stop here.
  virtual AiFilterStatus onRequestMetadata(Codec::AiRequest& /*req*/,
                                           AiFilterCallbacks& /*cb*/) {
    return AiFilterStatus::Continue;
  }

  // Q2+: per-item. Only invoked for kinds declared in itemInterest().
  // Runtime materializes the item before the call and re-stores on return
  // if the filter marked it dirty.
  virtual AiItemKindSet itemInterest() const { return AiItemKindSet::none(); }
  virtual AiFilterStatus onRequestItem(AiItem& /*item*/, AiFilterCallbacks& /*cb*/) {
    return AiFilterStatus::Continue;
  }

  // ======================== Response side =======================

  // R1: upstream response headers arrived. Scalars only — http_status,
  // response id / model echoed back, early metadata. Always invoked. No
  // chunk materialization.
  virtual AiFilterStatus onResponseStart(Codec::AiResponse& /*res*/,
                                         AiFilterCallbacks& /*cb*/) {
    return AiFilterStatus::Continue;
  }

  // R2: per-chunk, as chunks arrive from upstream. For streaming responses,
  // one call per SSE event / delta. For non-streaming responses, a single
  // call with kind=Final carrying the whole body. Only invoked for kinds
  // declared in chunkInterest() — chunks of unclaimed kinds pass through
  // to downstream without materialization.
  virtual AiChunkKindSet chunkInterest() const { return AiChunkKindSet::none(); }
  virtual AiFilterStatus onResponseChunk(Codec::AiResponseChunk& /*chunk*/,
                                         AiFilterCallbacks& /*cb*/) {
    return AiFilterStatus::Continue;
  }

  // R3: response complete. Final usage, finish_reason, trailers. Scalars
  // only. Always invoked after the chunk stream ends (including after a
  // single Final chunk for non-streaming).
  virtual AiFilterStatus onResponseEnd(Codec::AiResponse& /*res*/,
                                       AiFilterCallbacks& /*cb*/) {
    return AiFilterStatus::Continue;
  }

  virtual void onDestroy() {}
};

using AiFilterPtr = std::unique_ptr<AiFilter>;

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
