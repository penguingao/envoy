#include <deque>
#include <memory>
#include <string>

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

class AiProtocolManagerFilterTest : public testing::Test {
public:
  AiProtocolManagerFilterTest() {
    envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto;
    proto.set_best_effort_parsing(true);
    config_ = std::make_shared<AiProtocolManagerFilterConfig>(proto);
    filter_ = std::make_unique<AiProtocolManagerFilter>(factory_, config_);

    // Capture the upstream watermark callbacks the filter registers so tests can
    // simulate upstream back-pressure.
    ON_CALL(callbacks_, addUpstreamWatermarkCallbacks(testing::_))
        .WillByDefault(
            Invoke([this](Http::UpstreamWatermarkCallbacks& cb) { watermark_cb_ = &cb; }));
    ON_CALL(callbacks_, removeUpstreamWatermarkCallbacks(testing::_))
        .WillByDefault(
            Invoke([this](Http::UpstreamWatermarkCallbacks&) { watermark_cb_ = nullptr; }));
    // The manager creates a SchedulableCallback (to resume replay across event-loop
    // iterations) when setDecoderFilterCallbacks() builds it; construct the mock
    // first so it claims that call. The manager takes ownership of it.
    replay_cb_ = new NiceMock<Event::MockSchedulableCallback>(&callbacks_.dispatcher_);
    filter_->setDecoderFilterCallbacks(callbacks_);
    // The in-memory buffer delivers completions via dispatcher.post(). Capture
    // those callbacks so the test can run the event loop deterministically.
    ON_CALL(callbacks_.dispatcher_, post(testing::_))
        .WillByDefault(Invoke([this](Event::PostCb cb) { posted_.push_back(std::move(cb)); }));
    ON_CALL(callbacks_, injectDecodedDataToFilterChain(testing::_, testing::_))
        .WillByDefault(Invoke([this](Buffer::Instance& data, bool end_stream) {
          injected_.add(data);
          injected_end_stream_ = end_stream;
          ++inject_calls_;
          // Simulate upstream back-pressure arising mid-replay: when configured,
          // raise the watermark right after the Nth injected chunk.
          if (watermark_cb_ != nullptr && inject_calls_ == raise_watermark_at_inject_) {
            watermark_cb_->onAboveWriteBufferHighWatermark();
          }
        }));
    // Capture continueDecoding() so trailer-terminated streams can assert the
    // held trailers are released after the replayed body.
    ON_CALL(callbacks_, continueDecoding()).WillByDefault(Invoke([this]() { ++continue_calls_; }));
  }

  // The manager the filter owns requires onDestroy() before destruction (see
  // buffer_manager.h); Envoy guarantees that in production, so drive it here to
  // detach decode_manager_ before ~filter_ frees it. Idempotent, so tests that
  // already called onDestroy() are fine.
  void TearDown() override { filter_->onDestroy(); }

  // Run all posted callbacks, including ones enqueued while draining.
  void drain() {
    while (!posted_.empty()) {
      Event::PostCb cb = std::move(posted_.front());
      posted_.pop_front();
      cb();
    }
  }

  std::deque<Event::PostCb> posted_;
  InMemoryExternalBufferFactory factory_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> callbacks_;
  Http::UpstreamWatermarkCallbacks* watermark_cb_{};
  // Owned by the manager the filter builds; present so createSchedulableCallback()
  // returns a usable callback during construction.
  NiceMock<Event::MockSchedulableCallback>* replay_cb_{nullptr};
  AiProtocolManagerFilterConfigSharedPtr config_;
  std::unique_ptr<AiProtocolManagerFilter> filter_;

  Buffer::OwnedImpl injected_;
  bool injected_end_stream_{false};
  int inject_calls_{0};
  int continue_calls_{0};
  int raise_watermark_at_inject_{0}; // 0 = never.
};

// When a body follows the headers, iteration is paused so the rest of the chain
// (routing/admission) does not see the headers until the payload is offloaded.
TEST_F(AiProtocolManagerFilterTest, HoldsHeadersWhenBodyFollows) {
  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);
}

// A headers-only request has no payload to inspect, so the headers flow
// immediately; holding them would deadlock since no body drives the release.
TEST_F(AiProtocolManagerFilterTest, PassesHeadersOnlyRequest) {
  Http::TestRequestHeaderMapImpl headers{{":method", "GET"}, {":path", "/healthz"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, true), Http::FilterHeadersStatus::Continue);
}

// Headers held during decodeHeaders() are released when replay begins: the body
// is offloaded while iteration is paused, then injected back to the chain.
TEST_F(AiProtocolManagerFilterTest, ReleasesHeldHeadersOnReplay) {
  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl body("{\"messages\":[\"hi\"]}");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_GE(inject_calls_, 1);
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), "{\"messages\":[\"hi\"]}");
}

// The body is offloaded chunk-by-chunk and replayed verbatim once end_stream is
// seen, with end_stream propagated on the final injected frame.
TEST_F(AiProtocolManagerFilterTest, OffloadsAndReplaysBody) {
  Buffer::OwnedImpl chunk1("{\"messages\":");
  EXPECT_EQ(filter_->decodeData(chunk1, false), Http::FilterDataStatus::StopIterationNoBuffer);
  Buffer::OwnedImpl chunk2("[\"hi\"]}");
  EXPECT_EQ(filter_->decodeData(chunk2, true), Http::FilterDataStatus::StopIterationNoBuffer);

  // Nothing has been replayed yet: writes and replay are all asynchronous.
  EXPECT_EQ(inject_calls_, 0);

  drain();

  EXPECT_GE(inject_calls_, 1);
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), "{\"messages\":[\"hi\"]}");
}

// A body that arrives in a single end_stream frame is still round-tripped.
TEST_F(AiProtocolManagerFilterTest, SingleFrameBody) {
  Buffer::OwnedImpl body("{}");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), "{}");
}

// An empty terminal frame produces an empty end_stream marker downstream. It
// issues no write, so replay is scheduled (not started reentrantly from
// decodeData) and runs on the next event-loop iteration.
TEST_F(AiProtocolManagerFilterTest, EmptyBody) {
  Buffer::OwnedImpl empty;
  EXPECT_EQ(filter_->decodeData(empty, true), Http::FilterDataStatus::StopIterationNoBuffer);

  EXPECT_EQ(inject_calls_, 0);
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();

  EXPECT_EQ(inject_calls_, 1);
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.length(), 0);
}

// A payload larger than the replay chunk size is streamed back in multiple
// bounded frames and reassembles to the original bytes.
TEST_F(AiProtocolManagerFilterTest, LargePayloadReplayedInChunks) {
  const std::string big =
      "{\"data\":\"" + std::string(200 * 1024, 'x') + "\"}"; // > ReadChunkSize (64KiB)
  Buffer::OwnedImpl body(big);
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_GT(inject_calls_, 1);
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.length(), big.size());
  EXPECT_EQ(injected_.toString(), big);
}

// Destroying the filter mid-flight cancels pending callbacks; no replay occurs.
TEST_F(AiProtocolManagerFilterTest, DestroyBeforeReplay) {
  Buffer::OwnedImpl body("{\"key\":\"payload\"}");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  filter_->onDestroy();
  drain();

  EXPECT_EQ(inject_calls_, 0);
}

// The filter registers for upstream watermark callbacks so it can observe
// upstream back-pressure during replay.
TEST_F(AiProtocolManagerFilterTest, RegistersUpstreamWatermarkCallbacks) {
  EXPECT_NE(watermark_cb_, nullptr);
}

// When the downstream chain (toward upstream) is backed up before replay starts,
// no data is injected until the back-pressure is released.
TEST_F(AiProtocolManagerFilterTest, ReplayPausesUnderUpstreamBackPressure) {
  const std::string big =
      "{\"data\":\"" + std::string(200 * 1024, 'x') + "\"}"; // > ReadChunkSize, multiple chunks.
  Buffer::OwnedImpl body(big);
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);

  // Upstream signals back-pressure before replay begins.
  ASSERT_NE(watermark_cb_, nullptr);
  watermark_cb_->onAboveWriteBufferHighWatermark();

  // Draining completes the write and starts replay, but replay is paused: no
  // chunk is read or injected while the high watermark is held.
  drain();
  EXPECT_EQ(inject_calls_, 0);
  EXPECT_FALSE(injected_end_stream_);

  // Releasing back-pressure schedules the resume off the watermark callback stack
  // (deferred so we never read/inject reentrantly from within it); firing the
  // continuation runs replay to completion.
  watermark_cb_->onBelowWriteBufferLowWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.length(), big.size());
  EXPECT_EQ(injected_.toString(), big);
}

// Back-pressure arising mid-replay (the upstream fills as we inject) halts the
// synchronous read loop, and replay resumes when it clears.
TEST_F(AiProtocolManagerFilterTest, ReplayResumesMidStream) {
  const std::string big =
      "{\"data\":\"" + std::string(200 * 1024, 'x') + "\"}"; // 4 chunks of 64KiB + remainder.
  // Upstream backs up right after the first replayed chunk; the loop must then
  // stop until it is released.
  raise_watermark_at_inject_ = 1;
  Buffer::OwnedImpl body(big);
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);

  // Append completes, replay injects one chunk, then pauses on the watermark
  // raised during that inject.
  drain();
  EXPECT_EQ(inject_calls_, 1);
  EXPECT_FALSE(injected_end_stream_);
  EXPECT_LT(injected_.length(), big.size());

  // Release back-pressure; the resume is scheduled off the watermark callback
  // stack (deferred to avoid reentrant read/inject) and the continuation runs
  // replay to completion.
  ASSERT_NE(watermark_cb_, nullptr);
  watermark_cb_->onBelowWriteBufferLowWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), big);
}

// High watermark callbacks can nest (stream + connection); replay resumes only
// after a matching number of low-watermark callbacks.
TEST_F(AiProtocolManagerFilterTest, NestedWatermarksRequireBalancedRelease) {
  const std::string big = "{\"data\":\"" + std::string(200 * 1024, 'x') + "\"}";
  Buffer::OwnedImpl body(big);
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);

  ASSERT_NE(watermark_cb_, nullptr);
  watermark_cb_->onAboveWriteBufferHighWatermark();
  watermark_cb_->onAboveWriteBufferHighWatermark();
  drain();
  EXPECT_EQ(inject_calls_, 0);

  // One release is not enough to resume.
  watermark_cb_->onBelowWriteBufferLowWatermark();
  drain();
  EXPECT_EQ(inject_calls_, 0);
  EXPECT_FALSE(injected_end_stream_);

  // Balanced release schedules the resume off the watermark callback stack; the
  // continuation runs replay to completion.
  watermark_cb_->onBelowWriteBufferLowWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), big);
}

// A request whose body is terminated by trailers (last data frame has
// end_stream=false; the trailers carry END_STREAM) is not stuck: the body is
// replayed (final frame end_stream=false) and the held trailers are released
// via continueDecoding() once the body has been injected.
TEST_F(AiProtocolManagerFilterTest, TrailerTerminatedStream) {
  Buffer::OwnedImpl body("{\"messages\":[\"hi\"]}");
  EXPECT_EQ(filter_->decodeData(body, false), Http::FilterDataStatus::StopIterationNoBuffer);
  Http::TestRequestTrailerMapImpl trailers{{"x-trailer", "1"}};
  EXPECT_EQ(filter_->decodeTrailers(trailers), Http::FilterTrailersStatus::StopIteration);

  EXPECT_EQ(inject_calls_, 0);
  EXPECT_EQ(continue_calls_, 0);

  drain();

  EXPECT_GE(inject_calls_, 1);
  EXPECT_FALSE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), "{\"messages\":[\"hi\"]}");
  EXPECT_EQ(continue_calls_, 1);
}

// A trailer-only request (headers + trailers, no body) has nothing to offload, so
// the trailers flow immediately (Continue) and nothing is injected or held.
TEST_F(AiProtocolManagerFilterTest, TrailersWithoutBody) {
  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  Http::TestRequestTrailerMapImpl trailers{{"x-trailer", "1"}};
  EXPECT_EQ(filter_->decodeTrailers(trailers), Http::FilterTrailersStatus::Continue);
  drain();

  EXPECT_EQ(inject_calls_, 0);
  EXPECT_EQ(continue_calls_, 0);
}

// By default (no route config and best_effort_parsing = false), the filter does
// not parse JSON and does not offload the payload: headers, data, and trailers
// flow directly through the chain without being held or buffered.
TEST_F(AiProtocolManagerFilterTest, DefaultBehaviorPassesThroughWithoutParsingOrOffloading) {
  AiProtocolManagerFilter default_filter(factory_, nullptr);
  new NiceMock<Event::MockSchedulableCallback>(&callbacks_.dispatcher_);
  default_filter.setDecoderFilterCallbacks(callbacks_);

  EXPECT_CALL(callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/unconfigured/path"}};
  // Headers flow immediately (not held).
  EXPECT_EQ(default_filter.decodeHeaders(headers, false), Http::FilterHeadersStatus::Continue);

  Buffer::OwnedImpl body("not valid json at all {{}}");
  // Data flows immediately without offloading.
  EXPECT_EQ(default_filter.decodeData(body, true), Http::FilterDataStatus::Continue);
  drain();

  // No replay injection was needed since data was not drained/offloaded.
  EXPECT_EQ(inject_calls_, 0);
  EXPECT_EQ(default_filter.parsedDoc(), nullptr);

  Http::TestRequestTrailerMapImpl trailers;
  EXPECT_EQ(default_filter.decodeTrailers(trailers), Http::FilterTrailersStatus::Continue);

  default_filter.onDestroy();
}

// When route config is present, the filter populates parsedDoc() with structured JSON.
TEST_F(AiProtocolManagerFilterTest, PopulatesParsedDocOnValidJsonWithRouteConfig) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  AiProtocolManagerPerRouteConfig route_config(proto);
  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat/completions"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl body(R"({
    "model": "gpt-4o",
    "messages": [
      {"role": "user", "content": "hello world"}
    ]
  })");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  const JsonWithExtBuf* doc = filter_->parsedDoc();
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->json()["model"], "gpt-4o");
  EXPECT_EQ(doc->json()["messages"][0]["role"], "user");
  EXPECT_TRUE(JsonWithExtBuf::isOffloaded(doc->json()["messages"][0]["content"]));
}

// When route config is present, invalid JSON triggers a 400 Bad Request local reply.
TEST_F(AiProtocolManagerFilterTest, RejectsMalformedJsonWithBadRequestWhenRouteConfigured) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  AiProtocolManagerPerRouteConfig route_config(proto);
  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  EXPECT_CALL(callbacks_,
              sendLocalReply(Http::Code::BadRequest, testing::HasSubstr("Invalid JSON payload"),
                             testing::_, testing::_, testing::_));

  Buffer::OwnedImpl bad_body("{ invalid json");
  EXPECT_EQ(filter_->decodeData(bad_body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_EQ(inject_calls_, 0);
  EXPECT_EQ(filter_->parsedDoc(), nullptr);
}

// When route config is present, duplicate JSON keys trigger a 400 Bad Request local reply.
TEST_F(AiProtocolManagerFilterTest, RejectsDuplicateJsonKeysWithBadRequestWhenRouteConfigured) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  AiProtocolManagerPerRouteConfig route_config(proto);
  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  EXPECT_CALL(callbacks_,
              sendLocalReply(Http::Code::BadRequest, testing::HasSubstr("duplicate key"),
                             testing::_, testing::_, testing::_));

  Buffer::OwnedImpl dup_body(R"({"model": "gpt-4", "model": "gpt-3.5"})");
  EXPECT_EQ(filter_->decodeData(dup_body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_EQ(inject_calls_, 0);
}

// When route config is present, incomplete JSON on end_stream triggers a 400 Bad Request local
// reply.
TEST_F(AiProtocolManagerFilterTest, RejectsIncompleteJsonOnEndStreamWhenRouteConfigured) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  AiProtocolManagerPerRouteConfig route_config(proto);
  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/v1/chat"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  EXPECT_CALL(callbacks_,
              sendLocalReply(Http::Code::BadRequest, testing::HasSubstr("Invalid JSON payload"),
                             testing::_, testing::_, testing::_));

  Buffer::OwnedImpl incomplete_body(R"({"model": "gpt-4", "messages": )");
  EXPECT_EQ(filter_->decodeData(incomplete_body, true),
            Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_EQ(inject_calls_, 0);
}

// When route-specific configuration is matched (e.g. for /chat/completions), the filter
// stores the route config and strictly validates the JSON payload.
TEST_F(AiProtocolManagerFilterTest, RouteSpecificConfigMatchedPassThrough) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  proto.set_normalize(false);
  AiProtocolManagerPerRouteConfig route_config(proto);

  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/chat/completions"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl body(R"({"model":"gpt-4","messages":[{"role":"user","content":"hello"}]})");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  ASSERT_NE(filter_->routeConfig(), nullptr);
  EXPECT_EQ(filter_->routeConfig()->targetSchema(), "openai_chat");
  EXPECT_FALSE(filter_->routeConfig()->normalize());
  ASSERT_NE(filter_->parsedDoc(), nullptr);
  EXPECT_EQ(filter_->parsedDoc()->json()["model"], "gpt-4");
  EXPECT_GE(inject_calls_, 1);
}

TEST_F(AiProtocolManagerFilterTest, RouteSpecificConfigWithNormalize) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  proto.set_normalize(true);
  AiProtocolManagerPerRouteConfig route_config(proto);

  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/chat/completions"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl body(R"({"model":"gpt-4"})");
  EXPECT_EQ(filter_->decodeData(body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  ASSERT_NE(filter_->routeConfig(), nullptr);
  EXPECT_EQ(filter_->routeConfig()->targetSchema(), "openai_chat");
  EXPECT_TRUE(filter_->routeConfig()->normalize());
}

TEST_F(AiProtocolManagerFilterTest, RouteSpecificConfigRejectsMalformedJson) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute proto;
  proto.set_target_schema("openai_chat");
  proto.set_normalize(false);
  AiProtocolManagerPerRouteConfig route_config(proto);

  ON_CALL(callbacks_, mostSpecificPerFilterConfig()).WillByDefault(testing::Return(&route_config));

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/chat/completions"}};
  EXPECT_EQ(filter_->decodeHeaders(headers, false), Http::FilterHeadersStatus::StopIteration);

  EXPECT_CALL(callbacks_,
              sendLocalReply(Http::Code::BadRequest, testing::HasSubstr("Invalid JSON payload"),
                             testing::_, testing::_, testing::_));

  Buffer::OwnedImpl bad_body("not a json payload");
  EXPECT_EQ(filter_->decodeData(bad_body, true), Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_EQ(inject_calls_, 0);
  EXPECT_EQ(filter_->parsedDoc(), nullptr);
}

// When best_effort_parsing is enabled and no route-specific config is matched,
// malformed JSON is passed through without failing the request.
TEST_F(AiProtocolManagerFilterTest, BestEffortParsingPassesThroughMalformedJson) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto;
  proto.set_best_effort_parsing(true);
  auto filter_config = std::make_shared<AiProtocolManagerFilterConfig>(proto);

  AiProtocolManagerFilter best_effort_filter(factory_, filter_config);
  new NiceMock<Event::MockSchedulableCallback>(&callbacks_.dispatcher_);
  best_effort_filter.setDecoderFilterCallbacks(callbacks_);

  EXPECT_CALL(callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/other/endpoint"}};
  EXPECT_EQ(best_effort_filter.decodeHeaders(headers, false),
            Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl bad_body("plain text not json");
  EXPECT_EQ(best_effort_filter.decodeData(bad_body, true),
            Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  // Replay still succeeds and forwards the original payload!
  EXPECT_GE(inject_calls_, 1);
  EXPECT_TRUE(injected_end_stream_);
  EXPECT_EQ(injected_.toString(), "plain text not json");
  EXPECT_EQ(best_effort_filter.parsedDoc(), nullptr);

  best_effort_filter.onDestroy();
}

// When best_effort_parsing is enabled and JSON is valid, the parsed doc is populated.
TEST_F(AiProtocolManagerFilterTest, BestEffortParsingPopulatesValidJson) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto;
  proto.set_best_effort_parsing(true);
  auto filter_config = std::make_shared<AiProtocolManagerFilterConfig>(proto);

  AiProtocolManagerFilter best_effort_filter(factory_, filter_config);
  new NiceMock<Event::MockSchedulableCallback>(&callbacks_.dispatcher_);
  best_effort_filter.setDecoderFilterCallbacks(callbacks_);

  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}, {":path", "/other/endpoint"}};
  EXPECT_EQ(best_effort_filter.decodeHeaders(headers, false),
            Http::FilterHeadersStatus::StopIteration);

  Buffer::OwnedImpl body(R"({"status":"ok"})");
  EXPECT_EQ(best_effort_filter.decodeData(body, true),
            Http::FilterDataStatus::StopIterationNoBuffer);
  drain();

  EXPECT_GE(inject_calls_, 1);
  ASSERT_NE(best_effort_filter.parsedDoc(), nullptr);
  EXPECT_EQ(best_effort_filter.parsedDoc()->json()["status"], "ok");

  best_effort_filter.onDestroy();
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
