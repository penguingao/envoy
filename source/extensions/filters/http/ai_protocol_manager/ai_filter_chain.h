#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "envoy/event/schedulable_cb.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/leaf_awaitable.h"
#include "source/extensions/filters/ai/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/inference_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using AiFilters::AiFilterCallbacks;
using AiFilters::AiFilterPtr;
using AiFilters::PostDecodeAction;

// Runs a chain of AI filters over one request.
//
// Each filter is a coroutine, and they are all launched at once: a filter that
// has forwarded the request may keep working while the next one starts, so they
// are not simply run in sequence. What keeps that tractable is that exactly one
// filter holds the request at a time. Every other filter is suspended -- those
// ahead of it having forwarded, those behind it still waiting -- so the payload
// is handed along rather than shared, and no two filters can mutate it at once.
//
// Cross-coroutine transitions all happen in a single scheduled pump rather than
// directly from the awaitables. Two rules of the coroutine library force this:
// a leaf must not complete from within its own onStart() (the frame is
// mid-suspend), and completing a leaf resumes its coroutine inline. So the
// awaitables only ever *register*, and the pump is the one place that
// *completes* -- which also means there is a single place where reentrancy has
// to be reasoned about.
//
// Lifetime follows BufferManager's contract: onDestroy() detaches the chain and
// cancels every filter, and the owner frees it later. Cancellation unwinds each
// coroutine inline, so by the time onDestroy() returns no frame survives.
class AiFilterChain : public Logger::Loggable<Logger::Id::filter> {
public:
  // Invoked once every filter has finished, with what the chain decided.
  struct Outcome {
    // The payload as it emerged, or null if the chain never produced one.
    InferenceRequestPtr request;
    // Set when a filter asked to terminate the request.
    bool reset{false};
    // Non-OK if a filter failed.
    absl::Status status;
  };
  using DoneCallback = absl::AnyInvocable<void(Outcome)>;

  AiFilterChain(std::vector<AiFilterPtr> filters, AiFilterCallbacks& callbacks,
                Event::Dispatcher& dispatcher);
  ~AiFilterChain();

  // Starts every filter and hands `request` to the first. `done` runs once the
  // whole chain has finished. Call at most once.
  void start(InferenceRequestPtr request, DoneCallback done);

  // Detaches: cancels every filter coroutine and makes the pump inert. Must run
  // before destruction, and like BufferManager the object must stay alive until
  // its owner frees it -- a cancelled coroutine unwinds on this call's stack.
  void onDestroy();

  // True when there is nothing to run, so the caller can skip the machinery.
  bool empty() const { return slots_.empty(); }

private:
  // Awaitable handed to a filter waiting for the request.
  class GetAwaitable : public Coroutine::LeafAwaitable<absl::StatusOr<InferenceRequestPtr>> {
  public:
    GetAwaitable(AiFilterChain& chain, size_t index) : chain_(chain), index_(index) {}

    // complete() is protected on the base; this is how the pump -- the one place
    // allowed to resume a filter -- reaches it.
    void deliver(absl::StatusOr<InferenceRequestPtr> value) { complete(std::move(value)); }

  protected:
    void onStart() override { chain_.registerGet(index_, this); }
    void onCancel() override { chain_.clearGet(index_); }

  private:
    friend class AiFilterChain;
    AiFilterChain& chain_;
    const size_t index_;
  };

  // Awaitable handed to a filter that has passed the request on.
  class ForwardAwaitable : public Coroutine::LeafAwaitable<absl::Status> {
  public:
    ForwardAwaitable(AiFilterChain& chain, size_t index, InferenceRequestPtr request)
        : chain_(chain), index_(index), request_(std::move(request)) {}

    void deliver(absl::Status status) { complete(std::move(status)); }

  protected:
    void onStart() override { chain_.registerForward(index_, std::move(request_), this); }
    void onCancel() override { chain_.clearForward(index_); }

  private:
    friend class AiFilterChain;
    AiFilterChain& chain_;
    const size_t index_;
    InferenceRequestPtr request_;
  };

  // The getter/forwarder pair a filter is handed. Held by the chain so they
  // outlive the coroutine that awaits them.
  class Getter : public AiFilters::InferenceRequestGetter {
  public:
    Getter(AiFilterChain& chain, size_t index) : chain_(chain), index_(index) {}
    Coroutine::Task<absl::StatusOr<InferenceRequestPtr>> get() override;

  private:
    AiFilterChain& chain_;
    const size_t index_;
  };

  class Forwarder : public AiFilters::InferenceRequestForwarder {
  public:
    Forwarder(AiFilterChain& chain, size_t index) : chain_(chain), index_(index) {}
    Coroutine::Task<absl::Status> forward(InferenceRequestPtr request) override;

  private:
    AiFilterChain& chain_;
    const size_t index_;
  };

  // Per-filter state. At most one of the two leaves is registered at a time,
  // which mirrors the coroutine library's own "one suspended leaf per context"
  // invariant and is asserted on registration.
  struct Slot {
    explicit Slot(AiFilterPtr f) : filter(std::move(f)) {}

    AiFilterPtr filter;
    std::unique_ptr<Getter> getter;
    std::unique_ptr<Forwarder> forwarder;
    std::optional<Coroutine::DetachedHandle> handle;

    GetAwaitable* get_leaf{nullptr};
    ForwardAwaitable* forward_leaf{nullptr};

    // The request waiting to be handed to this filter.
    InferenceRequestPtr inbox;
    bool has_inbox{false};

    bool forwarded{false};
    bool finished{false};
  };

  // Awaitable registration, called from onStart()/onCancel().
  void registerGet(size_t index, GetAwaitable* leaf);
  void clearGet(size_t index);
  void registerForward(size_t index, InferenceRequestPtr request, ForwardAwaitable* leaf);
  void clearForward(size_t index);

  // Hands `request` to filter `index`, or to the chain's end.
  void deposit(size_t index, InferenceRequestPtr request);

  // Reclaims a request a filter dropped rather than forwarding, and passes it on
  // in the filter's place.
  void onRequestDropped(size_t index, InferenceRequest* request);

  // Records a filter's result. Only records and schedules: it can run while a
  // cancelled coroutine is unwinding.
  void onFilterDone(size_t index, absl::StatusOr<PostDecodeAction> result);

  // The one place leaves are completed. Runs until no filter can make progress.
  void pump();

  // Fires the caller's completion once every filter has finished.
  void maybeFinish();

  std::vector<Slot> slots_;
  AiFilterCallbacks& callbacks_;
  std::shared_ptr<Coroutine::Executor> executor_;
  Event::SchedulableCallbackPtr pump_cb_;

  // The payload once it has passed the last filter.
  InferenceRequestPtr result_;
  bool result_ready_{false};

  DoneCallback done_;
  size_t outstanding_{0};
  bool started_{false};
  bool reset_requested_{false};
  absl::Status status_;
  bool destroyed_{false};
};
using AiFilterChainPtr = std::unique_ptr<AiFilterChain>;

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
