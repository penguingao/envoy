#pragma once

#include <memory>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Forward declarations from codec (response side, implemented later).
namespace Codec {
struct AiResponse;
struct AiResponseChunk;
struct AiItem;
} // namespace Codec

namespace Chain {

class AiFilterCallbacks;

enum class AiFilterStatus {
  Continue,       // advance to next filter in the current phase
  StopIteration,  // pause; resume via AiFilterCallbacks::continueRequest/Response
};

// Bitset: which item kinds this filter wants onRequestItem calls for.
// Filters that declare none() are skipped during item iteration — their
// onRequestItem is never called, preventing unnecessary PayloadStore::fetch.
struct AiItemKindSet {
  bool messages{false};
  bool tools{false};
  bool attachments{false};

  static AiItemKindSet all()  { return {true, true, true}; }
  static AiItemKindSet none() { return {false, false, false}; }
  bool any() const { return messages || tools || attachments; }
};

// Bitset: which response chunk kinds this filter wants onResponseChunk for.
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

  static AiChunkKindSet all()  {
    return {true, true, true, true, true, true, true, true, true, true};
  }
  static AiChunkKindSet none() { return {}; }
  bool any() const {
    return started || item_added || content_delta || reasoning_delta ||
           tool_call_delta || item_done || completed || error_event ||
           final_body || raw;
  }
};

// AiFilter — the operator-facing filter interface for the AI sub-chains.
//
// The chain is phase-major: each phase completes across the entire chain before
// the next begins (Q1 for all → Q2 per item for all → dispatch → R1 for all …).
// Filters implement only the phases they care about; all defaults are no-op
// Continue, so a metadata-only filter compiles with just onRequestMetadata().
class AiFilter {
public:
  virtual ~AiFilter() = default;

  // ═════════════════════════ Request side ═════════════════════════════════

  // Phase Q1 — scalars only, always invoked.
  // Sees the envelope and variant's scalar fields. Most cross-cutting filters
  // (rate limit, budget, model routing) stop here.
  virtual AiFilterStatus onRequestMetadata(Codec::AiRequest&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase Q2 — per-item, iterated across messages / tools / attachments.
  // itemInterest() declares which kinds the filter wants; the runtime skips
  // materializing items of unclaimed kinds entirely.
  virtual AiItemKindSet itemInterest() const { return AiItemKindSet::none(); }
  virtual AiFilterStatus onRequestItem(Codec::AiItem&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // ═════════════════════════ Response side ════════════════════════════════

  // Phase R1 — response headers arrived. Scalars only: http_status, id, model.
  virtual AiFilterStatus onResponseStart(Codec::AiResponse&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase R2 — per streaming chunk. chunkInterest() controls materialisation.
  virtual AiChunkKindSet chunkInterest() const { return AiChunkKindSet::none(); }
  virtual AiFilterStatus onResponseChunk(Codec::AiResponseChunk&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase R3 — response complete. Final usage / finish_reason / trailers.
  virtual AiFilterStatus onResponseEnd(Codec::AiResponse&, AiFilterCallbacks&) {
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
