#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_callbacks.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// AiFilterChain runs the phased state machine over an ordered list of AiFilters.
//
// Analogy to FilterManager
// ─────────────────────────
//  FilterManager::decoder_filters_          →  AiFilterChain::filters_
//  FilterManager::decodeHeaders(...)        →  AiFilterChain::runRequestMetadata(req)
//  FilterManager::decodeData(...)           →  AiFilterChain::runRequestItem(item)
//  ActiveStreamDecoderFilter::continueDecoding() → AiFilterCallbacksImpl::continueRequest()
//
// Phase-major ordering
// ─────────────────────
//  Q1 onRequestMetadata : f0 → f1 → … → fN
//  Q2 onRequestItem(m0) : f0 → f1 → … → fN
//  Q2 onRequestItem(m1) : f0 → f1 → … → fN   (one pass per item)
//  …
//  ── dispatch ──
//  R1 onResponseStart   : f0 → f1 → … → fN
//  R2 onResponseChunk(0): f0 → f1 → … → fN   (one pass per chunk)
//  …
//  R3 onResponseEnd     : f0 → f1 → … → fN
//
// Pause / resume
// ──────────────
// A filter returning StopIteration pauses the chain at (phase, filter_index).
// AiFilterCallbacks::continueRequest/Response() resumes from the next filter
// in the same phase, exactly as FilterManager::commonContinue() does.
class AiFilterChain : public AiFilterChainCallbacks,
                      public Logger::Loggable<Logger::Id::filter> {
public:
  // Completion callbacks — fired by the chain when each phase boundary is
  // crossed (i.e. all filters in the phase returned Continue). The outer
  // AiProtocolManagerFilter wires these up so it knows when to trigger dispatch
  // or when to forward the re-encoded response.
  using OnRequestReady  = std::function<void(Codec::AiRequest&)>;
  using OnResponseReady = std::function<void()>;

  // Fired when a filter in Q1/Q2 calls AiFilterCallbacks::sendLocalReply().
  // The outer filter sends the HTTP response via decoder_callbacks_ and skips
  // dispatch entirely — mirroring how HTTP filter short-circuits work.
  using OnLocalReply = std::function<void(Codec::AiResponse&&)>;

  AiFilterChain() = default;
  ~AiFilterChain() override;

  // AiFilterChainCallbacks — called during chain construction.
  void addAiFilter(AiFilterPtr filter) override;

  // Compute and cache the union of declared interests across all filters.
  // Must be called after all addAiFilter() calls and before any run*() calls.
  void finalizeInterests();

  // ── Request-side entry points ─────────────────────────────────────────────

  // Phase Q1: run onRequestMetadata for all filters in order.
  // `on_ready` is invoked after the last filter returns Continue (or after
  // the last filter that stopped has resumed via continueRequest()).
  // `on_local_reply` (optional) is invoked instead of on_ready when any filter
  // calls sendLocalReply() — the outer filter then skips dispatch and sends the
  // HTTP response directly.
  void runRequestMetadata(Codec::AiRequest& req, OnRequestReady on_ready,
                          OnLocalReply on_local_reply = nullptr);

  // Phase Q2: run onRequestItem for all filters that declared interest.
  // Called once per item. `on_ready` is invoked after fN returns Continue.
  void runRequestItem(Codec::AiItem& item, OnRequestReady on_ready);

  // ── Response-side entry points ────────────────────────────────────────────

  void runResponseStart(Codec::AiResponse& resp, OnResponseReady on_ready);
  void runResponseChunk(Codec::AiResponseChunk& chunk, OnResponseReady on_ready);
  void runResponseEnd(Codec::AiResponse& resp, OnResponseReady on_ready);

  // Combined interest queries — result of finalizeInterests().
  const AiItemKindSet&  combinedItemInterest()  const { return combined_item_interest_;  }
  const AiChunkKindSet& combinedChunkInterest() const { return combined_chunk_interest_; }

private:
  // ── Inner callbacks implementation ────────────────────────────────────────
  class CallbacksImpl;

  // ── Phase-major iteration helpers ─────────────────────────────────────────

  // Run `invoke` across filters starting at `start_idx`. Returns true if all
  // filters returned Continue; false if one stopped (paused).
  enum class Phase { RequestMetadata, RequestItem, ResponseStart, ResponseChunk, ResponseEnd };

  template <typename Payload, typename InvokeFn>
  bool iterateFrom(size_t start_idx, Phase phase, Payload& payload, InvokeFn&& invoke);

  // ── State ──────────────────────────────────────────────────────────────────

  std::vector<AiFilterPtr> filters_;

  // Pause state — set when a filter returns StopIteration.
  Phase  paused_phase_{Phase::RequestMetadata};
  size_t paused_at_{0};          // index of the filter that paused
  bool   paused_{false};

  // Callbacks pointer set for the duration of each run* call so that
  // continueRequest/Response() can re-enter the loop.
  CallbacksImpl* active_callbacks_{nullptr};

  // Cached union of interest declarations.
  AiItemKindSet  combined_item_interest_;
  AiChunkKindSet combined_chunk_interest_;
  bool           interests_finalized_{false};
};

using AiFilterChainPtr = std::unique_ptr<AiFilterChain>;

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
