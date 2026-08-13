#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/ai_filter_chain.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using AiFilters::AiFilter;
using AiFilters::AiFilterCallbacks;
using AiFilters::AiFilterPtr;
using AiFilters::InferenceRequestForwarder;
using AiFilters::InferenceRequestGetter;
using AiFilters::PostDecodeAction;
using testing::NiceMock;

// Minimal callbacks: the chain only passes these through to filters.
class FakeCallbacks : public AiFilterCallbacks {
public:
  Http::RequestHeaderMapOptRef requestHeaders() override { return {headers_}; }
  void sendLocalReply(Http::Code code, absl::string_view, absl::string_view details) override {
    local_reply_code_ = code;
    local_reply_details_ = std::string(details);
  }
  StreamInfo::StreamInfo& streamInfo() override { return stream_info_; }
  Event::Dispatcher& dispatcher() override { return *dispatcher_; }

  Http::TestRequestHeaderMapImpl headers_{{":path", "/chat/completions"}};
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;
  Event::Dispatcher* dispatcher_{nullptr};
  std::optional<Http::Code> local_reply_code_;
  std::string local_reply_details_;
};

// A filter whose behavior each test scripts. Every ordering assertion is made
// through `log`, which records what happened and in what order.
class ScriptedFilter : public AiFilter {
public:
  enum class Behavior {
    // Await the request, forward it, return.
    Forward,
    // Await the request and return without forwarding: the chain must pass it
    // on rather than strand it.
    DropWithoutForwarding,
    // Forward, then ask for the request to be reset.
    ForwardThenReset,
    // Return an error instead of forwarding.
    Fail,
    // Await the request and never return: used to observe cancellation.
    HangHoldingRequest,
  };

  ScriptedFilter(std::string name, Behavior behavior, std::vector<std::string>& log)
      : name_(std::move(name)), behavior_(behavior), log_(log) {}

  void setCallbacks(AiFilterCallbacks&) override {}
  void onDestroy() override { log_.push_back(name_ + ":destroy"); }

  Coroutine::Task<absl::StatusOr<PostDecodeAction>>
  decode(InferenceRequestGetter& getter, InferenceRequestForwarder& forwarder) override {
    log_.push_back(name_ + ":start");
    absl::StatusOr<InferenceRequestPtr> request = co_await getter.get();
    if (!request.ok()) {
      log_.push_back(name_ + ":cancelled");
      co_return request.status();
    }
    log_.push_back(name_ + ":got");

    if (behavior_ == Behavior::Fail) {
      co_return absl::InternalError(name_ + " failed");
    }
    if (behavior_ == Behavior::DropWithoutForwarding) {
      // Letting the pointer go out of scope is the whole point of this case.
      co_return PostDecodeAction::Skip;
    }
    if (behavior_ == Behavior::HangHoldingRequest) {
      // Awaiting a second time never resolves: the request has been forwarded
      // so nothing will hand it back.
      absl::StatusOr<InferenceRequestPtr> again = co_await getter.get();
      if (!again.ok()) {
        log_.push_back(name_ + ":cancelled");
        co_return again.status();
      }
      log_.push_back(name_ + ":resumed");
      co_return PostDecodeAction::Skip;
    }

    // Mark the payload so a test can prove which filters saw it.
    (*request)->mutableJson()["seen_by"].push_back(name_);

    const absl::Status status = co_await forwarder.forward(std::move(*request));
    if (!status.ok()) {
      log_.push_back(name_ + ":forward_failed");
      co_return status;
    }
    log_.push_back(name_ + ":forwarded");
    co_return behavior_ == Behavior::ForwardThenReset ? PostDecodeAction::Reset
                                                      : PostDecodeAction::Skip;
  }

private:
  const std::string name_;
  const Behavior behavior_;
  std::vector<std::string>& log_;
};

class AiFilterChainTest : public testing::Test {
public:
  AiFilterChainTest() {
    callbacks_.dispatcher_ = &dispatcher_;
    ON_CALL(dispatcher_, post(testing::_)).WillByDefault(testing::Invoke([this](Event::PostCb cb) {
      posted_.push_back(std::move(cb));
    }));
  }

  void TearDown() override {
    if (chain_ != nullptr) {
      chain_->onDestroy();
    }
  }

  void build(std::vector<std::pair<std::string, ScriptedFilter::Behavior>> spec) {
    // Created before the chain so it outlives nothing that matters, but after
    // the dispatcher mock is armed.
    pump_cb_ = new NiceMock<Event::MockSchedulableCallback>(&dispatcher_);
    std::vector<AiFilterPtr> filters;
    for (auto& [name, behavior] : spec) {
      filters.push_back(std::make_unique<ScriptedFilter>(name, behavior, log_));
    }
    chain_ = std::make_unique<AiFilterChain>(std::move(filters), callbacks_, dispatcher_);
  }

  InferenceRequestPtr makeRequest() {
    JsonWithExtBuf payload;
    nlohmann::json root = nlohmann::json::object();
    root["model"] = "gpt-4";
    root["seen_by"] = nlohmann::json::array();
    payload.setJson(std::move(root));
    return makeInferenceRequest(std::move(payload));
  }

  void start() {
    chain_->start(makeRequest(), [this](AiFilterChain::Outcome outcome) {
      finished_ = true;
      outcome_ = std::move(outcome);
    });
  }

  // Drives the scheduled pump and the posted queue until neither has work.
  void drive() {
    for (int i = 0; i < 100; ++i) {
      while (!posted_.empty()) {
        Event::PostCb cb = std::move(posted_.front());
        posted_.pop_front();
        cb();
      }
      if (!pump_cb_->enabled()) {
        return;
      }
      pump_cb_->invokeCallback();
    }
    ADD_FAILURE() << "chain did not settle";
  }

  std::vector<std::string> seenBy() const {
    std::vector<std::string> names;
    if (outcome_.request == nullptr) {
      return names;
    }
    for (const auto& name : outcome_.request->json().at("seen_by")) {
      names.push_back(name.get<std::string>());
    }
    return names;
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  std::deque<Event::PostCb> posted_;
  NiceMock<Event::MockSchedulableCallback>* pump_cb_{nullptr};
  FakeCallbacks callbacks_;
  std::vector<std::string> log_;
  AiFilterChainPtr chain_;
  bool finished_{false};
  AiFilterChain::Outcome outcome_;
};

// The payload reaches every filter, in chain order, and comes out the far end.
TEST_F(AiFilterChainTest, PassesRequestAlongInOrder) {
  build({{"a", ScriptedFilter::Behavior::Forward},
         {"b", ScriptedFilter::Behavior::Forward},
         {"c", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  ASSERT_NE(outcome_.request, nullptr);
  EXPECT_THAT(seenBy(), testing::ElementsAre("a", "b", "c"));
  EXPECT_FALSE(outcome_.reset);
  EXPECT_TRUE(outcome_.status.ok());
}

// A filter only receives the request after the one before it forwarded, which
// is what "one filter holds it at a time" means.
TEST_F(AiFilterChainTest, HandOffIsSerialized) {
  build({{"a", ScriptedFilter::Behavior::Forward}, {"b", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  const auto a_forwarded = std::find(log_.begin(), log_.end(), "a:forwarded");
  const auto b_got = std::find(log_.begin(), log_.end(), "b:got");
  ASSERT_NE(a_forwarded, log_.end());
  ASSERT_NE(b_got, log_.end());
  // b sees the payload only once a has handed it on.
  EXPECT_LT(std::distance(log_.begin(), a_forwarded), std::distance(log_.begin(), b_got) + 1);
  EXPECT_EQ(log_.front(), "a:start");
}

// An empty chain is a pass-through rather than a special case for the caller.
TEST_F(AiFilterChainTest, EmptyChainPassesThrough) {
  build({});
  EXPECT_TRUE(chain_->empty());
  start();
  drive();

  ASSERT_TRUE(finished_);
  ASSERT_NE(outcome_.request, nullptr);
  EXPECT_TRUE(seenBy().empty());
}

TEST_F(AiFilterChainTest, SingleFilter) {
  build({{"only", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  EXPECT_THAT(seenBy(), testing::ElementsAre("only"));
}

// A filter that returns still holding the request must not strand it: the chain
// forwards on its behalf, which is what the interface promises.
TEST_F(AiFilterChainTest, DroppedRequestIsForwardedByTheChain) {
  build({{"a", ScriptedFilter::Behavior::Forward},
         {"dropper", ScriptedFilter::Behavior::DropWithoutForwarding},
         {"c", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  ASSERT_NE(outcome_.request, nullptr);
  // The dropper never marked the payload, but c still saw it.
  EXPECT_THAT(seenBy(), testing::ElementsAre("a", "c"));
}

// A dropper at the end of the chain still yields the payload to the caller.
TEST_F(AiFilterChainTest, DroppedRequestAtChainEnd) {
  build({{"a", ScriptedFilter::Behavior::Forward},
         {"last", ScriptedFilter::Behavior::DropWithoutForwarding}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  ASSERT_NE(outcome_.request, nullptr);
  EXPECT_THAT(seenBy(), testing::ElementsAre("a"));
}

// A reset is reported to the caller, which is what decides how to end the
// stream.
TEST_F(AiFilterChainTest, ReportsReset) {
  build({{"a", ScriptedFilter::Behavior::ForwardThenReset},
         {"b", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  EXPECT_TRUE(outcome_.reset);
}

// A failing filter's status reaches the caller.
TEST_F(AiFilterChainTest, ReportsFilterFailure) {
  build({{"bad", ScriptedFilter::Behavior::Fail}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  EXPECT_FALSE(outcome_.status.ok());
  EXPECT_EQ(outcome_.status.code(), absl::StatusCode::kInternal);
}

// A filter that fails while holding the request does not strand it either.
TEST_F(AiFilterChainTest, FailureStillReleasesRequest) {
  build({{"a", ScriptedFilter::Behavior::Forward}, {"bad", ScriptedFilter::Behavior::Fail}});
  start();
  drive();

  ASSERT_TRUE(finished_);
  EXPECT_FALSE(outcome_.status.ok());
  // The payload came back rather than being lost with the failing frame.
  ASSERT_NE(outcome_.request, nullptr);
}

// Completion waits for every filter, not just for the payload to reach the end:
// a filter may still be working after forwarding.
TEST_F(AiFilterChainTest, WaitsForEveryFilterToFinish) {
  build({{"a", ScriptedFilter::Behavior::Forward},
         {"hanger", ScriptedFilter::Behavior::HangHoldingRequest}});
  start();
  drive();

  // hanger forwarded nothing and never returned, so the chain is not done.
  EXPECT_FALSE(finished_);
}

// Teardown mid-flight cancels every filter, each exactly once, and leaves no
// waiter registered.
TEST_F(AiFilterChainTest, DestroyCancelsPendingFilters) {
  build({{"a", ScriptedFilter::Behavior::Forward},
         {"hanger", ScriptedFilter::Behavior::HangHoldingRequest},
         {"c", ScriptedFilter::Behavior::Forward}});
  start();
  drive();
  ASSERT_FALSE(finished_);

  chain_->onDestroy();

  // The hanging filter was resumed with a cancellation and unwound; the filter
  // that never received the payload was cancelled at its getter.
  EXPECT_EQ(std::count(log_.begin(), log_.end(), "hanger:cancelled"), 1);
  EXPECT_EQ(std::count(log_.begin(), log_.end(), "c:cancelled"), 1);
  // The caller is not called back after teardown.
  EXPECT_FALSE(finished_);
}

// onDestroy() is idempotent, since the owner may detach and then tear down.
TEST_F(AiFilterChainTest, DestroyIsIdempotent) {
  build({{"a", ScriptedFilter::Behavior::Forward}});
  start();
  chain_->onDestroy();
  chain_->onDestroy();
  EXPECT_EQ(std::count(log_.begin(), log_.end(), "a:destroy"), 1);
}

// Tearing down a chain that already completed is harmless.
TEST_F(AiFilterChainTest, DestroyAfterCompletion) {
  build({{"a", ScriptedFilter::Behavior::Forward}});
  start();
  drive();
  ASSERT_TRUE(finished_);

  chain_->onDestroy();
  EXPECT_TRUE(outcome_.status.ok());
}

// A filter that modified the payload marks it, which is what selects
// re-serialization over replaying the original bytes.
TEST_F(AiFilterChainTest, ModificationMarksPayloadDirty) {
  build({{"a", ScriptedFilter::Behavior::Forward}});
  start();
  drive();

  ASSERT_NE(outcome_.request, nullptr);
  EXPECT_TRUE(outcome_.request->dirty());
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
