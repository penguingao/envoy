#include <memory>
#include <string>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/serializer.h"

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

class SerializerTest : public testing::Test {
public:
  SerializerTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test")), factory_(),
        bridge_(std::make_unique<FakeBridge>(*dispatcher_)),
        buffer_manager_(factory_, std::move(bridge_)) {}

  ~SerializerTest() override { buffer_manager_.onDestroy(); }

  void drain() { dispatcher_->run(Event::Dispatcher::RunType::NonBlock); }

  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  InMemoryExternalBufferFactory factory_;
  std::unique_ptr<FakeBridge> bridge_;
  BufferManager buffer_manager_;
};

TEST_F(SerializerTest, PureJsonSerialization) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"temperature", 0.7},
      {"stream", false},
      {"tags", {"a", "b", "c"}},
  });

  auto result_or = Serializer::serialize(doc, nullptr);
  ASSERT_OK(result_or);
  std::string output = result_or->toString();

  auto parsed = nlohmann::json::parse(output);
  EXPECT_EQ(parsed["model"], "gpt-4");
  EXPECT_DOUBLE_EQ(parsed["temperature"].get<double>(), 0.7);
  EXPECT_EQ(parsed["stream"], false);
  EXPECT_EQ(parsed["tags"], (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(SerializerTest, ExternalRefSerialization) {
  std::string secret = "This is a very long offloaded prompt text.";
  Buffer::OwnedImpl secret_buf(secret);
  buffer_manager_.onData(secret_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "claude-3"},
      {"prompt", JsonWithExtBuf::makeExternalRef({0, secret.size()})},
  });

  auto result_or = Serializer::serialize(doc, &buffer_manager_);
  ASSERT_OK(result_or);
  std::string output = result_or->toString();

  auto parsed = nlohmann::json::parse(output);
  EXPECT_EQ(parsed["model"], "claude-3");
  EXPECT_EQ(parsed["prompt"], secret);
}

TEST_F(SerializerTest, ExternalRefNullBufferFails) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"prompt", JsonWithExtBuf::makeExternalRef({0, 10})},
  });

  auto result_or = Serializer::serialize(doc, nullptr);
  EXPECT_THAT(result_or.status(), HasStatusCode(absl::StatusCode::kInternal));
}

TEST_F(SerializerTest, NestedStructureWithMultipleRefs) {
  std::string part1 = "System instructions";
  std::string part2 = "User query text";

  Buffer::OwnedImpl part1_buf(part1);
  buffer_manager_.onData(part1_buf);
  Buffer::OwnedImpl part2_buf(part2);
  buffer_manager_.onData(part2_buf);
  buffer_manager_.endStream();
  drain();

  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"messages",
       nlohmann::json::array({
           {{"role", "system"}, {"content", JsonWithExtBuf::makeExternalRef({0, part1.size()})}},
           {{"role", "user"},
            {"content", JsonWithExtBuf::makeExternalRef({part1.size(), part2.size()})}},
       })},
  });

  auto result_or = Serializer::serialize(doc, &buffer_manager_);
  ASSERT_OK(result_or);
  std::string output = result_or->toString();

  auto parsed = nlohmann::json::parse(output);
  ASSERT_TRUE(parsed["messages"].is_array());
  EXPECT_EQ(parsed["messages"][0]["content"], part1);
  EXPECT_EQ(parsed["messages"][1]["content"], part2);
}

class FakeSinkProvider : public SinkProvider {
public:
  bool hasFieldStream(absl::string_view path) const override { return streams_.contains(path); }

  Coroutine::Task<absl::StatusOr<std::optional<FieldStream>>>
  getFieldStream(absl::string_view path) override {
    auto it = streams_.find(path);
    if (it != streams_.end()) {
      auto buf = std::move(it->second);
      streams_.erase(it);
      co_return std::make_optional(std::move(buf));
    }
    co_return std::nullopt;
  }

  absl::flat_hash_map<std::string, FieldStream> streams_;
};

} // namespace

class FieldStreamPeer {
public:
  static void pushChunk(FieldStream& buf, Buffer::OwnedImpl chunk, bool end_stream) {
    buf.pushChunk(std::move(chunk), end_stream);
  }
};

namespace {

TEST_F(SerializerTest, StreamingSerializationWithSinkProvider) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"prompt", JsonWithExtBuf::makeExternalRef({0, 100})},
  });

  FakeSinkProvider sink;
  FieldStream buf("/prompt");
  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("Hello "), false);
  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("world!"), true);
  sink.streams_.emplace("/prompt", std::move(buf));

  auto executor = std::make_shared<Coroutine::DispatcherExecutor>(*dispatcher_);
  Buffer::OwnedImpl collected;
  bool completed = false;

  auto task = Serializer::serialize(
      doc, &buffer_manager_, [&collected](Buffer::Instance& chunk, bool) { collected.add(chunk); },
      &sink);

  auto handle = Coroutine::launch(
      std::move(task), executor,
      [&completed](absl::Status status) {
        ASSERT_OK(status);
        completed = true;
      },
      Coroutine::StartMode::Inline);

  drain();
  EXPECT_TRUE(completed);

  auto parsed = nlohmann::json::parse(collected.toString());
  EXPECT_EQ(parsed["model"], "gpt-4");
  EXPECT_EQ(parsed["prompt"], "Hello world!");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
