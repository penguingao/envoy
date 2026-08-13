#include "source/extensions/filters/http/ai_protocol_manager/ai_filter_chain.h"

#include <utility>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

Coroutine::Task<absl::StatusOr<InferenceRequestPtr>> AiFilterChain::Getter::get() {
  co_return co_await GetAwaitable(chain_, index_);
}

Coroutine::Task<absl::Status> AiFilterChain::Forwarder::forward(InferenceRequestPtr request) {
  co_return co_await ForwardAwaitable(chain_, index_, std::move(request));
}

AiFilterChain::AiFilterChain(std::vector<AiFilterPtr> filters, AiFilterCallbacks& callbacks,
                             Event::Dispatcher& dispatcher)
    : callbacks_(callbacks),
      executor_(std::make_shared<Coroutine::DispatcherExecutor>(dispatcher)) {
  slots_.reserve(filters.size());
  for (AiFilterPtr& filter : filters) {
    slots_.emplace_back(std::move(filter));
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    slots_[i].getter = std::make_unique<Getter>(*this, i);
    slots_[i].forwarder = std::make_unique<Forwarder>(*this, i);
    slots_[i].filter->setCallbacks(callbacks_);
  }
  pump_cb_ = dispatcher.createSchedulableCallback([this]() { pump(); });
}

AiFilterChain::~AiFilterChain() { ASSERT(destroyed_ || slots_.empty()); }

void AiFilterChain::start(InferenceRequestPtr request, DoneCallback done) {
  ASSERT(!started_);
  started_ = true;
  done_ = std::move(done);

  if (slots_.empty()) {
    // Nothing to run: the payload passes through untouched.
    result_ = std::move(request);
    result_ready_ = true;
    maybeFinish();
    return;
  }

  outstanding_ = slots_.size();
  deposit(0, std::move(request));

  for (size_t i = 0; i < slots_.size(); ++i) {
    // StartMode::Inline rather than Scheduled: the executor's schedule() posts,
    // and a post cannot be cancelled, so a root launched but not yet started
    // would still resume after onDestroy(). Starting inline means every root is
    // already suspended at its first await by the time this returns, and
    // cancellation can reach all of them.
    //
    // Inline also means on_done can fire before launch() returns, so the slot
    // must be able to absorb a result before its handle is assigned.
    slots_[i].handle = Coroutine::launch(
        slots_[i].filter->decode(*slots_[i].getter, *slots_[i].forwarder), executor_,
        [this, i](absl::StatusOr<PostDecodeAction> result) { onFilterDone(i, std::move(result)); },
        Coroutine::StartMode::Inline);
  }

  // Every filter is now parked on its getter; the pump hands the request to the
  // first one. Deferred so nothing resumes on the caller's stack.
  if (!destroyed_) {
    pump_cb_->scheduleCallbackCurrentIteration();
  }
}

void AiFilterChain::onDestroy() {
  if (destroyed_) {
    return;
  }
  destroyed_ = true;
  pump_cb_->cancel();
  // Cancelling resumes each pending leaf with an aborted status, so the filter
  // unwinds and its frame is destroyed on this stack. By index rather than by
  // iterator: onFilterDone() runs during this loop.
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].handle.has_value()) {
      slots_[i].handle->cancel();
    }
  }
  for (Slot& slot : slots_) {
    if (slot.filter != nullptr) {
      slot.filter->onDestroy();
    }
  }
  done_ = nullptr;
}

void AiFilterChain::registerGet(size_t index, GetAwaitable* leaf) {
  Slot& slot = slots_[index];
  // One suspended leaf per filter: the chain's own restatement of the coroutine
  // library's invariant, which a filter awaiting two things at once would break.
  ASSERT(slot.get_leaf == nullptr && slot.forward_leaf == nullptr);
  slot.get_leaf = leaf;
  if (!destroyed_) {
    pump_cb_->scheduleCallbackCurrentIteration();
  }
}

void AiFilterChain::clearGet(size_t index) { slots_[index].get_leaf = nullptr; }

void AiFilterChain::registerForward(size_t index, InferenceRequestPtr request,
                                    ForwardAwaitable* leaf) {
  Slot& slot = slots_[index];
  ASSERT(slot.get_leaf == nullptr && slot.forward_leaf == nullptr);
  slot.forwarded = true;
  slot.forward_leaf = leaf;
  // Hand the payload on immediately; the forwarding filter is released by the
  // pump, so it never observes the next filter running.
  deposit(index + 1, std::move(request));
  if (!destroyed_) {
    pump_cb_->scheduleCallbackCurrentIteration();
  }
}

void AiFilterChain::clearForward(size_t index) { slots_[index].forward_leaf = nullptr; }

void AiFilterChain::deposit(size_t index, InferenceRequestPtr request) {
  if (request == nullptr) {
    return;
  }
  if (index >= slots_.size()) {
    result_ = std::move(request);
    result_ready_ = true;
    return;
  }
  // Re-own it so that dropping it inside the filter comes back to us rather
  // than destroying the payload.
  InferenceRequest* raw = request.release();
  slots_[index].inbox =
      InferenceRequestPtr(raw, InferenceRequestDisposer([this, index](InferenceRequest* dropped) {
                            onRequestDropped(index, dropped);
                          }));
  slots_[index].has_inbox = true;
}

void AiFilterChain::onRequestDropped(size_t index, InferenceRequest* request) {
  if (destroyed_) {
    delete request;
    return;
  }
  // The filter returned still holding the payload. Rather than strand it, pass
  // it along in the filter's place -- which is what makes forwarding a question
  // of when, not whether.
  ENVOY_LOG(debug, "ai_protocol_manager: filter {} released the request without forwarding", index);
  InferenceRequestPtr reclaimed(request, InferenceRequestDisposer());
  deposit(index + 1, std::move(reclaimed));
  pump_cb_->scheduleCallbackCurrentIteration();
}

void AiFilterChain::onFilterDone(size_t index, absl::StatusOr<PostDecodeAction> result) {
  // Only record and schedule: this can run while a cancelled coroutine unwinds
  // on onDestroy()'s stack, where acting would re-enter a chain being torn down.
  ASSERT(outstanding_ > 0);
  --outstanding_;
  slots_[index].finished = true;

  if (destroyed_) {
    return;
  }
  if (!result.ok()) {
    if (status_.ok()) {
      status_ = result.status();
    }
  } else if (*result == PostDecodeAction::Reset) {
    reset_requested_ = true;
  }
  pump_cb_->scheduleCallbackCurrentIteration();
}

void AiFilterChain::pump() {
  if (destroyed_) {
    return;
  }
  // Completing a leaf resumes its filter inline, which can register the next
  // leaf, so keep sweeping until nothing moved.
  bool progressed = true;
  while (progressed) {
    progressed = false;
    for (size_t i = 0; i < slots_.size(); ++i) {
      Slot& slot = slots_[i];

      if (slot.get_leaf != nullptr && slot.has_inbox) {
        GetAwaitable* leaf = slot.get_leaf;
        slot.get_leaf = nullptr;
        slot.has_inbox = false;
        leaf->deliver(std::move(slot.inbox));
        if (destroyed_) {
          return;
        }
        progressed = true;
      }

      if (slot.forward_leaf != nullptr) {
        ForwardAwaitable* leaf = slot.forward_leaf;
        slot.forward_leaf = nullptr;
        leaf->deliver(absl::OkStatus());
        if (destroyed_) {
          return;
        }
        progressed = true;
      }
    }
  }
  maybeFinish();
}

void AiFilterChain::maybeFinish() {
  if (destroyed_ || outstanding_ > 0 || done_ == nullptr) {
    return;
  }
  // Every filter has returned. A chain that produced no payload -- every filter
  // failed or was cancelled before forwarding -- reports that rather than
  // pretending it has one.
  Outcome outcome;
  outcome.request = std::move(result_);
  outcome.reset = reset_requested_;
  outcome.status = status_;
  result_ready_ = false;

  DoneCallback done = std::move(done_);
  done_ = nullptr;
  done(std::move(outcome));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
