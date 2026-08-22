#include <memory>
#include <string>
#include <vector>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/status_macros.h"
#include "source/extensions/filters/http/ai_protocol_manager/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_streaming_session.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema.h"

#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using ::Envoy::StatusHelpers::HasStatusCode;

class FakeBridge : public FilterChainBridge {
public:
  explicit FakeBridge(Event::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}

  Event::Dispatcher& dispatcher() override { return dispatcher_; }
  uint32_t bufferLimit() override { return 1024 * 1024; }
  void injectData(Buffer::Instance&) override {}
  void pauseSource() override {}
  void resumeSource() override {}
  void registerReplayWatermarks(ReplayWatermarkHandler&) override {}
  void unregisterReplayWatermarks() override {}
  void onUnrecoverableError() override {}

  Event::Dispatcher& dispatcher_;
};

class FilterManagerTest : public testing::Test {
public:
  FilterManagerTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test")), factory_(),
        bridge_(std::make_unique<FakeBridge>(*dispatcher_)),
        buffer_manager_(factory_, std::move(bridge_)) {}

  ~FilterManagerTest() override { buffer_manager_.onDestroy(); }

  void drain() {
    for (int i = 0; i < 20; ++i) {
      dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
    }
  }

  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  InMemoryExternalBufferFactory factory_;
  std::unique_ptr<FakeBridge> bridge_;
  BufferManager buffer_manager_;
};

// 0-filter pass-through
TEST_F(FilterManagerTest, ZeroFilterPassThrough) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{{"model", "gpt-4"}});

  std::vector<AiFilterPtr> filters;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_);

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);
  EXPECT_EQ(manager.requestJson().json()["model"], "gpt-4");
}

// Custom mock filter for testing
class TestMutationFilter : public AiFilter {
public:
  explicit TestMutationFilter(std::string target_model) : target_model_(std::move(target_model)) {}

  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->doc().json()["model"] = target_model_;

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      while (buf_opt.has_value()) {
        session->publish(*buf_opt);
        ASSIGN_OR_CO_RETURN(buf_opt, co_await session->fetch());
      }
    }
    co_return absl::OkStatus();
  }

private:
  std::string target_model_;
};

TEST_F(FilterManagerTest, SingleFilterPass1Mutation) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{{"model", "gpt-3.5"}});

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestMutationFilter>("gpt-4o-mini"));

  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_);

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);
  EXPECT_EQ(manager.requestJson().json()["model"], "gpt-4o-mini");
}

// Pass 2 chunk streaming filter
class TestStreamTransformFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());

    // Register interest in /prompt
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      while (buf_opt.has_value()) {
        FieldStreamHandle& handle = *buf_opt;
        while (true) {
          auto chunk_or = co_await handle.recv();
          if (!chunk_or.ok() || !chunk_or->has_value()) {
            break;
          }
          std::string upper = (*chunk_or)->toString();
          for (char& c : upper) {
            c = static_cast<char>(std::toupper(c));
          }
          Buffer::OwnedImpl upper_buf(upper);
          CO_RETURN_IF_ERROR(co_await handle.forward(std::move(upper_buf), false));
        }
        Buffer::OwnedImpl empty;
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(empty), true));
        session->publish(handle);
        ASSIGN_OR_CO_RETURN(buf_opt, co_await session->fetch());
      }
    }

    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, SingleFilterPass2Streaming) {
  std::string raw_prompt = "hello world from ai filter";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestStreamTransformFilter>());

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["model"], "gpt-4");
  EXPECT_EQ(parsed["prompt"], "HELLO WORLD FROM AI FILTER");
}

// Field dropping filter: receives field via session->fetch(), but does NOT call session->publish()
class TestFieldDroppingFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/system_prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      while (buf_opt.has_value()) {
        // Intentionally drop the field by consuming but not calling session->publish()
        FieldStreamHandle& handle = *buf_opt;
        while (true) {
          auto chunk_or = co_await handle.recv();
          if (!chunk_or.ok() || !chunk_or->has_value()) {
            break;
          }
        }
        ASSIGN_OR_CO_RETURN(buf_opt, co_await session->fetch());
      }
    }

    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, FieldDroppingOmitsFieldFromOutput) {
  std::string secret = "top secret internal instruction";
  Buffer::OwnedImpl secret_buf(secret);
  buffer_manager_.onData(secret_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"system_prompt", JsonWithExtBuf::makeExternalRef({0, secret.size()})},
      {"user_prompt", "normal question"},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestFieldDroppingFilter>());

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["model"], "gpt-4");
  EXPECT_FALSE(parsed.contains("system_prompt"));
  EXPECT_EQ(parsed["user_prompt"], "normal question");
}

// Multi-filter chaining: Filter1 -> Filter2
class TestAppendFilter : public AiFilter {
public:
  explicit TestAppendFilter(std::string suffix) : suffix_(std::move(suffix)) {}

  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      while (buf_opt.has_value()) {
        FieldStreamHandle& handle = *buf_opt;
        while (true) {
          auto chunk_or = co_await handle.recv();
          if (!chunk_or.ok()) {
            break;
          }
          if (!chunk_or->has_value()) {
            Buffer::OwnedImpl s_buf(suffix_);
            CO_RETURN_IF_ERROR(co_await handle.forward(std::move(s_buf), true));
            break;
          }
          CO_RETURN_IF_ERROR(co_await handle.forward(std::move(**chunk_or), false));
        }

        session->publish(handle);
        ASSIGN_OR_CO_RETURN(buf_opt, co_await session->fetch());
      }
    }

    co_return absl::OkStatus();
  }

private:
  std::string suffix_;
};

TEST_F(FilterManagerTest, MultiFilterChainingPipeline) {
  std::string raw_prompt = "start";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestAppendFilter>(" -> filter1"));
  filters.push_back(std::make_unique<TestAppendFilter>(" -> filter2"));

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["prompt"], "start -> filter1 -> filter2");
}

// Error propagation test
class TestErrorFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter, AiRequestForwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    co_return absl::PermissionDeniedError("access blocked by policy filter");
  }
};

TEST_F(FilterManagerTest, FilterErrorPropagation) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{{"model", "gpt-4"}});

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestErrorFilter>());

  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_);

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  EXPECT_THAT(status, HasStatusCode(absl::StatusCode::kPermissionDenied));
  EXPECT_EQ(status.message(), "access blocked by policy filter");
}

// OnDestroy cancellation test
TEST_F(FilterManagerTest, OnDestroyCancelsInFlightChain) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{{"model", "gpt-4"}});

  std::vector<AiFilterPtr> filters;
  // A filter that never finishes until explicitly cancelled
  class HangingFilter : public AiFilter {
  public:
    Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter, AiRequestForwarder) override {
      ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
      // Await on a hanging buffer recv
      FieldStream hanging("/hanging");
      auto chunk_or = co_await hanging.recv();
      (void)chunk_or;
      co_return absl::OkStatus();
    }
  };

  filters.push_back(std::make_unique<HangingFilter>());

  auto manager = std::make_unique<FilterManager>(std::move(filters), std::move(doc),
                                                 &buffer_manager_, *dispatcher_);

  bool completed = false;
  manager->start([&completed](absl::Status) { completed = true; });

  drain();
  EXPECT_FALSE(completed);

  // Calling onDestroy cancels running tasks
  manager->onDestroy();
  drain();
  EXPECT_FALSE(completed);
}

// Two-pass filter: inspects prompt in pass 1, then transforms prompt in pass 2
class TestTwoPassFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());

    // Pass 1: register for streaming inspection
    req->registerFieldForStreaming("/prompt");
    auto p1_session = req->stream();
    if (p1_session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await p1_session->fetch());
      if (buf_opt.has_value()) {
        FieldStreamHandle& p1_handle = *buf_opt;
        std::string inspected;
        while (true) {
          auto chunk_or = co_await p1_handle.recv();
          if (!chunk_or.ok() || !chunk_or->has_value()) {
            break;
          }
          inspected += (*chunk_or)->toString();
        }
        if (inspected == "check_me") {
          req->doc().json()["inspected"] = true;
        }
      }
    }

    // Pass 2: register for transformation
    req->registerFieldForStreaming("/prompt");
    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> p2_session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (p2_session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await p2_session->fetch());
      if (buf_opt.has_value()) {
        FieldStreamHandle& p2_handle = *buf_opt;
        Buffer::OwnedImpl prefix("transformed: ");
        CO_RETURN_IF_ERROR(co_await p2_handle.forward(std::move(prefix), false));

        while (true) {
          auto chunk_or = co_await p2_handle.recv();
          if (!chunk_or.ok() || !chunk_or->has_value()) {
            break;
          }
          CO_RETURN_IF_ERROR(co_await p2_handle.forward(std::move(**chunk_or), false));
        }
        Buffer::OwnedImpl empty;
        CO_RETURN_IF_ERROR(co_await p2_handle.forward(std::move(empty), true));
        p2_session->publish(p2_handle);
      }
    }

    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, Pass1InspectionThenPass2Transformation) {
  std::string raw_prompt = "check_me";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestTwoPassFilter>());

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["model"], "gpt-4");
  EXPECT_EQ(parsed["inspected"], true);
  EXPECT_EQ(parsed["prompt"], "transformed: check_me");
}

class TestRetainAccessFilter : public AiFilter {
public:
  explicit TestRetainAccessFilter(std::string* captured_path, bool* eof_verified)
      : captured_path_(captured_path), eof_verified_(eof_verified) {}

  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      if (buf_opt.has_value()) {
        FieldStreamHandle& handle = *buf_opt;
        // Transform chunks
        Buffer::OwnedImpl greeting("hello ");
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(greeting), false));

        while (true) {
          auto chunk_or = co_await handle.recv();
          if (!chunk_or.ok() || !chunk_or->has_value()) {
            break;
          }
          CO_RETURN_IF_ERROR(co_await handle.forward(std::move(**chunk_or), false));
        }
        Buffer::OwnedImpl empty;
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(empty), true));

        // Publish to session
        session->publish(handle);

        // Filter retains access to handle after calling publish()
        if (captured_path_ != nullptr) {
          *captured_path_ = std::string(handle.jsonPath());
        }
        auto post_forward_recv = co_await handle.recv();
        if (eof_verified_ != nullptr && post_forward_recv.ok() && !post_forward_recv->has_value()) {
          *eof_verified_ = true;
        }
      }
    }
    co_return absl::OkStatus();
  }

private:
  std::string* captured_path_;
  bool* eof_verified_;
};

TEST_F(FilterManagerTest, FilterRetainsBufferAccessAfterForward) {
  std::string raw_prompt = "world";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::string captured_path;
  bool eof_verified = false;
  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestRetainAccessFilter>(&captured_path, &eof_verified));

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  EXPECT_EQ(captured_path, "/prompt");
  EXPECT_TRUE(eof_verified);
  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["prompt"], "hello world");
}

class TestEarlyForwardFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      if (buf_opt.has_value()) {
        FieldStreamHandle& handle = *buf_opt;
        // Indicate to session that this buffer should be visible to next filter immediately
        session->publish(handle);

        // Push chunks through handle after calling publish()
        Buffer::OwnedImpl c1("realtime ");
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(c1), false));
        Buffer::OwnedImpl c2("streaming");
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(c2), false));
        Buffer::OwnedImpl empty;
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(empty), true));
      }
    }
    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, EarlyForwardRealtimeStreaming) {
  std::string raw_prompt = "ignored_initial_val";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestEarlyForwardFilter>());
  filters.push_back(std::make_unique<TestAppendFilter>(" -> filter2"));

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["prompt"], "realtime streaming -> filter2");
}

class TestEarlyReturnWithoutStreamingFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    // Early return without consuming or forwarding chunks from session
    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, FilterEarlyReturnsWithoutConsumingStream) {
  std::string raw_prompt = "original_prompt_data";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestEarlyReturnWithoutStreamingFilter>());
  filters.push_back(std::make_unique<TestAppendFilter>(" -> filter2"));

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["prompt"], "original_prompt_data -> filter2");
}

class TestEarlyReturnAfterPartialStreamFilter : public AiFilter {
public:
  Coroutine::Task<absl::Status> decode(AiRequestGetter req_getter,
                                       AiRequestForwarder req_forwarder) override {
    ASSIGN_OR_CO_RETURN(std::unique_ptr<AiRequest> req, co_await std::move(req_getter)());
    req->registerFieldForStreaming("/prompt");

    ASSIGN_OR_CO_RETURN(std::unique_ptr<FieldStreamingSession> session,
                        co_await std::move(req_forwarder)(std::move(req)));

    if (session != nullptr) {
      ASSIGN_OR_CO_RETURN(auto buf_opt, co_await session->fetch());
      if (buf_opt.has_value()) {
        FieldStreamHandle& handle = *buf_opt;
        session->publish(handle);

        // Forward a prefix chunk, then early return before receiving upstream EOF
        Buffer::OwnedImpl prefix("prefix_");
        CO_RETURN_IF_ERROR(co_await handle.forward(std::move(prefix), false));
      }
    }
    // Early return
    co_return absl::OkStatus();
  }
};

TEST_F(FilterManagerTest, FilterEarlyReturnsAfterPartialStream) {
  std::string raw_prompt = "body_data";
  Buffer::OwnedImpl raw_buf(raw_prompt);
  buffer_manager_.onData(raw_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"prompt", JsonWithExtBuf::makeExternalRef({0, raw_prompt.size()})},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<TestEarlyReturnAfterPartialStreamFilter>());
  filters.push_back(std::make_unique<TestAppendFilter>(" -> filter2"));

  Buffer::OwnedImpl injected;
  FilterManager manager(std::move(filters), std::move(doc), &buffer_manager_, *dispatcher_, nullptr,
                        [&injected](Buffer::Instance& data, bool) { injected.add(data); });

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();
  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  auto parsed = nlohmann::json::parse(injected.toString());
  EXPECT_EQ(parsed["prompt"], "prefix_body_data -> filter2");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
