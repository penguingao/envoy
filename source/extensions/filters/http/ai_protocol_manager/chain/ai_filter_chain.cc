#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// ─────────────────────────────────────────────────────────────────────────────
// CallbacksImpl — concrete AiFilterCallbacks bound to a running chain phase.
//
// One CallbacksImpl is stack-allocated for each run*() call and held by
// AiFilterChain::active_callbacks_. Its continueRequest() / continueResponse()
// re-enters the phase loop from the filter after the one that paused.
// ─────────────────────────────────────────────────────────────────────────────

class AiFilterChain::CallbacksImpl : public AiFilterCallbacks {
public:
  CallbacksImpl(AiFilterChain& chain, Event::Dispatcher& dispatcher,
                StreamInfo::StreamInfo& stream_info, const AiProtocolManagerConfig& cfg,
                AiFilterChain::Phase phase, AiFilterChain::OnRequestReady* req_ready,
                AiFilterChain::OnResponseReady* resp_ready,
                AiFilterChain::OnLocalReply* local_reply_cb = nullptr)
      : chain_(chain), dispatcher_(dispatcher), stream_info_(stream_info), config_(cfg),
        phase_(phase), req_ready_(req_ready), resp_ready_(resp_ready),
        local_reply_cb_(local_reply_cb) {}

  Event::Dispatcher& dispatcher() override { return dispatcher_; }
  StreamInfo::StreamInfo& streamInfo() override { return stream_info_; }
  const AiProtocolManagerConfig& config() override { return config_; }

  // ── Continuation ───────────────────────────────────────────────────────────

  void continueRequest() override {
    ASSERT(phase_ == Phase::RequestMetadata || phase_ == Phase::RequestItem);
    ASSERT(chain_.paused_);
    chain_.paused_ = false;
    // Resume from the filter after the one that called StopIteration.
    size_t resume_from = chain_.paused_at_ + 1;
    bool done = chain_.iterateFrom(resume_from, phase_, *current_req_payload_, [&](AiFilter& f) {
      return invokeRequestPhase(f);
    });
    if (done && req_ready_) {
      (*req_ready_)(*current_req_payload_);
    }
  }

  void continueResponse() override {
    ASSERT(phase_ == Phase::ResponseStart || phase_ == Phase::ResponseChunk ||
           phase_ == Phase::ResponseEnd);
    ASSERT(chain_.paused_);
    chain_.paused_ = false;
    size_t resume_from = chain_.paused_at_ + 1;
    bool done = chain_.iterateFrom(resume_from, phase_, *current_resp_payload_, [&](AiFilter& f) {
      return invokeResponsePhase(f);
    });
    if (done && resp_ready_) {
      (*resp_ready_)();
    }
  }

  // ── Short-circuit ──────────────────────────────────────────────────────────

  void sendLocalReply(Codec::AiResponse&& resp) override {
    pending_local_reply_ = std::move(resp);
    local_reply_sent_    = true;
    // The outer AiProtocolManagerFilter's OnLocalReply callback handles the
    // actual HTTP response via decoder_callbacks_->sendLocalReply(). It is
    // invoked by runRequestMetadata() after iterateFrom() returns.
  }

  void endResponseEarly(Codec::AiResponse&&) override {
    // TODO: drain encode buffer, emit synthetic tail downstream.
    response_ended_early_ = true;
  }

  // ── Per-item / per-chunk ───────────────────────────────────────────────────

  void dropCurrentItem() override    { item_dropped_    = true; }
  void insertAfterItem(Codec::AiItem&&) override  { /* TODO: item insertion queue */ }
  void dropCurrentChunk() override   { chunk_dropped_   = true; }
  void insertAfterChunk(Codec::AiResponseChunk&&) override { /* TODO */ }

  void recordEvent(AiEvent) override { /* TODO: stats */ }

  // ── Payload pointers (set by the chain before each filter invocation) ──────

  // Request-side payload pointer — set by runRequestMetadata / runRequestItem.
  Codec::AiRequest* current_req_payload_{nullptr};
  // Response-side payload pointer — set by runResponseStart / etc.
  // Using void* because AiResponse and AiResponseChunk are not yet defined in
  // this translation unit; the concrete run* methods cast appropriately.
  void* current_resp_payload_{nullptr};

  // Termination flags.
  bool local_reply_sent_{false};
  bool response_ended_early_{false};
  bool item_dropped_{false};
  bool chunk_dropped_{false};

  // Populated by sendLocalReply(); consumed by runRequestMetadata().
  Codec::AiResponse pending_local_reply_;

private:
  AiFilterStatus invokeRequestPhase(AiFilter& f) {
    switch (phase_) {
    case Phase::RequestMetadata:
      return f.onRequestMetadata(*current_req_payload_, *this);
    case Phase::RequestItem:
      return f.onRequestItem(*static_cast<Codec::AiItem*>(current_resp_payload_), *this);
    default:
      PANIC("unexpected phase in invokeRequestPhase");
    }
  }

  AiFilterStatus invokeResponsePhase(AiFilter& f) {
    switch (phase_) {
    case Phase::ResponseStart:
      return f.onResponseStart(*static_cast<Codec::AiResponse*>(current_resp_payload_), *this);
    case Phase::ResponseChunk:
      return f.onResponseChunk(*static_cast<Codec::AiResponseChunk*>(current_resp_payload_), *this);
    case Phase::ResponseEnd:
      return f.onResponseEnd(*static_cast<Codec::AiResponse*>(current_resp_payload_), *this);
    default:
      PANIC("unexpected phase in invokeResponsePhase");
    }
  }

  AiFilterChain&                  chain_;
  Event::Dispatcher&              dispatcher_;
  StreamInfo::StreamInfo&         stream_info_;
  const AiProtocolManagerConfig&  config_;
  Phase                           phase_;
  AiFilterChain::OnRequestReady*  req_ready_;
  AiFilterChain::OnResponseReady* resp_ready_;
  AiFilterChain::OnLocalReply*    local_reply_cb_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AiFilterChain
// ─────────────────────────────────────────────────────────────────────────────

AiFilterChain::~AiFilterChain() {
  for (auto& f : filters_) {
    f->onDestroy();
  }
}

void AiFilterChain::addAiFilter(AiFilterPtr filter) {
  ASSERT(!interests_finalized_, "addAiFilter() called after finalizeInterests()");
  filters_.push_back(std::move(filter));
}

void AiFilterChain::finalizeInterests() {
  combined_item_interest_  = AiItemKindSet::none();
  combined_chunk_interest_ = AiChunkKindSet::none();
  for (const auto& f : filters_) {
    const auto ii = f->itemInterest();
    combined_item_interest_.messages    |= ii.messages;
    combined_item_interest_.tools       |= ii.tools;
    combined_item_interest_.attachments |= ii.attachments;

    const auto ci = f->chunkInterest();
    combined_chunk_interest_.started        |= ci.started;
    combined_chunk_interest_.item_added     |= ci.item_added;
    combined_chunk_interest_.content_delta  |= ci.content_delta;
    combined_chunk_interest_.reasoning_delta|= ci.reasoning_delta;
    combined_chunk_interest_.tool_call_delta|= ci.tool_call_delta;
    combined_chunk_interest_.item_done      |= ci.item_done;
    combined_chunk_interest_.completed      |= ci.completed;
    combined_chunk_interest_.error_event    |= ci.error_event;
    combined_chunk_interest_.final_body     |= ci.final_body;
    combined_chunk_interest_.raw            |= ci.raw;
  }
  interests_finalized_ = true;
}

// ── Generic phase-major iteration ────────────────────────────────────────────
//
// Returns true if the entire range [start_idx, filters_.size()) ran to
// completion (all returned Continue). Returns false and sets paused_ if any
// filter returned StopIteration.
template <typename Payload, typename InvokeFn>
bool AiFilterChain::iterateFrom(size_t start_idx, Phase phase, Payload& payload,
                                InvokeFn&& invoke) {
  for (size_t i = start_idx; i < filters_.size(); ++i) {
    AiFilter& f = *filters_[i];
    AiFilterStatus status = invoke(f);
    if (status == AiFilterStatus::StopIteration) {
      paused_      = true;
      paused_at_   = i;
      paused_phase_ = phase;
      return false;
    }
  }
  return true;
}

// ── Request side ──────────────────────────────────────────────────────────────

void AiFilterChain::runRequestMetadata(Codec::AiRequest& req, OnRequestReady on_ready,
                                       OnLocalReply on_local_reply) {
  ASSERT(interests_finalized_);
  ASSERT(!paused_);

  // Stack-allocate callbacks for this run.
  // TODO: pass real dispatcher/stream_info/config from the outer filter.
  CallbacksImpl cb(*this, *static_cast<Event::Dispatcher*>(nullptr),
                   *static_cast<StreamInfo::StreamInfo*>(nullptr),
                   *static_cast<const AiProtocolManagerConfig*>(nullptr),
                   Phase::RequestMetadata, &on_ready, nullptr, &on_local_reply);
  cb.current_req_payload_ = &req;
  active_callbacks_ = &cb;

  bool done = iterateFrom(0, Phase::RequestMetadata, req, [&](AiFilter& f) -> AiFilterStatus {
    cb.current_req_payload_ = &req;
    return f.onRequestMetadata(req, cb);
  });

  active_callbacks_ = nullptr;

  if (cb.local_reply_sent_) {
    // A filter called sendLocalReply() — propagate to the outer filter so it
    // can issue the HTTP response and skip dispatch entirely.
    if (on_local_reply) {
      on_local_reply(std::move(cb.pending_local_reply_));
    }
    return;
  }

  if (done) {
    on_ready(req);
  }
  // If !done (and no local reply): the chain is paused for async work;
  // continueRequest() will resume iteration and eventually fire on_ready.
}

void AiFilterChain::runRequestItem(Codec::AiItem& item, OnRequestReady on_ready) {
  ASSERT(interests_finalized_);
  ASSERT(!paused_);

  CallbacksImpl cb(*this, *static_cast<Event::Dispatcher*>(nullptr),
                   *static_cast<StreamInfo::StreamInfo*>(nullptr),
                   *static_cast<const AiProtocolManagerConfig*>(nullptr),
                   Phase::RequestItem, &on_ready, nullptr);
  cb.current_resp_payload_ = &item;  // AiItem passed via resp_payload slot
  active_callbacks_ = &cb;

  bool done = iterateFrom(0, Phase::RequestItem, item, [&](AiFilter& f) -> AiFilterStatus {
    if (!f.itemInterest().any()) {
      return AiFilterStatus::Continue;
    }
    return f.onRequestItem(item, cb);
  });

  active_callbacks_ = nullptr;
  if (done) {
    on_ready(*cb.current_req_payload_);
  }
}

// ── Response side ─────────────────────────────────────────────────────────────

void AiFilterChain::runResponseStart(Codec::AiResponse& resp, OnResponseReady on_ready) {
  ASSERT(interests_finalized_);

  CallbacksImpl cb(*this, *static_cast<Event::Dispatcher*>(nullptr),
                   *static_cast<StreamInfo::StreamInfo*>(nullptr),
                   *static_cast<const AiProtocolManagerConfig*>(nullptr),
                   Phase::ResponseStart, nullptr, &on_ready);
  cb.current_resp_payload_ = &resp;
  active_callbacks_ = &cb;

  bool done = iterateFrom(0, Phase::ResponseStart, resp, [&](AiFilter& f) -> AiFilterStatus {
    return f.onResponseStart(resp, cb);
  });

  active_callbacks_ = nullptr;
  if (done) {
    on_ready();
  }
}

void AiFilterChain::runResponseChunk(Codec::AiResponseChunk& chunk, OnResponseReady on_ready) {
  ASSERT(interests_finalized_);

  CallbacksImpl cb(*this, *static_cast<Event::Dispatcher*>(nullptr),
                   *static_cast<StreamInfo::StreamInfo*>(nullptr),
                   *static_cast<const AiProtocolManagerConfig*>(nullptr),
                   Phase::ResponseChunk, nullptr, &on_ready);
  cb.current_resp_payload_ = &chunk;
  active_callbacks_ = &cb;

  bool done = iterateFrom(0, Phase::ResponseChunk, chunk, [&](AiFilter& f) -> AiFilterStatus {
    if (!f.chunkInterest().any()) {
      return AiFilterStatus::Continue;
    }
    return f.onResponseChunk(chunk, cb);
  });

  active_callbacks_ = nullptr;
  if (done) {
    on_ready();
  }
}

void AiFilterChain::runResponseEnd(Codec::AiResponse& resp, OnResponseReady on_ready) {
  ASSERT(interests_finalized_);

  CallbacksImpl cb(*this, *static_cast<Event::Dispatcher*>(nullptr),
                   *static_cast<StreamInfo::StreamInfo*>(nullptr),
                   *static_cast<const AiProtocolManagerConfig*>(nullptr),
                   Phase::ResponseEnd, nullptr, &on_ready);
  cb.current_resp_payload_ = &resp;
  active_callbacks_ = &cb;

  bool done = iterateFrom(0, Phase::ResponseEnd, resp, [&](AiFilter& f) -> AiFilterStatus {
    return f.onResponseEnd(resp, cb);
  });

  active_callbacks_ = nullptr;
  if (done) {
    on_ready();
  }
}

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
