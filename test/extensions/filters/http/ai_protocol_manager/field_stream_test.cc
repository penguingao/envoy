#include <memory>
#include <string>
#include <vector>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/task.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_stream.h"

#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class FieldStreamPeer {
public:
  static void pushChunk(FieldStream& buf, Buffer::OwnedImpl chunk, bool end_stream) {
    buf.pushChunk(std::move(chunk), end_stream);
  }
  static void close(FieldStream& buf) { buf.close(); }
};

namespace {

using ::Envoy::StatusHelpers::HasStatusCode;

class FieldStreamTest : public testing::Test {
public:
  FieldStreamTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test")),
        executor_(std::make_shared<Coroutine::DispatcherExecutor>(*dispatcher_)) {}

  void drain() { dispatcher_->run(Event::Dispatcher::RunType::NonBlock); }

  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  std::shared_ptr<Coroutine::DispatcherExecutor> executor_;
};

TEST_F(FieldStreamTest, PathAccessor) {
  FieldStream buf("/prompt");
  EXPECT_EQ(buf.jsonPath(), "/prompt");
}

TEST_F(FieldStreamTest, PushBeforeRecv) {
  FieldStream buf("/prompt");
  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("chunk1"), false);
  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("chunk2"), true);

  bool completed = false;
  auto task = [](FieldStream& b, bool& done) -> Coroutine::Task<absl::Status> {
    auto c1_or = co_await b.recv();
    if (!c1_or.ok()) {
      co_return c1_or.status();
    }
    EXPECT_TRUE(c1_or->has_value());
    EXPECT_EQ((*c1_or)->toString(), "chunk1");

    auto c2_or = co_await b.recv();
    if (!c2_or.ok()) {
      co_return c2_or.status();
    }
    EXPECT_TRUE(c2_or->has_value());
    EXPECT_EQ((*c2_or)->toString(), "chunk2");

    auto c3_or = co_await b.recv();
    if (!c3_or.ok()) {
      co_return c3_or.status();
    }
    EXPECT_FALSE(c3_or->has_value());

    done = true;
    co_return absl::OkStatus();
  }(buf, completed);

  auto handle = Coroutine::launch(
      std::move(task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);
  EXPECT_TRUE(completed);
}

TEST_F(FieldStreamTest, RecvSuspensionAndResume) {
  FieldStream buf("/messages/0/content");

  std::vector<std::string> received;
  bool completed = false;

  auto task = [](FieldStream& b, std::vector<std::string>& out,
                 bool& done) -> Coroutine::Task<absl::Status> {
    while (true) {
      auto chunk_or = co_await b.recv();
      if (!chunk_or.ok()) {
        co_return chunk_or.status();
      }
      if (!chunk_or->has_value()) {
        break;
      }
      out.push_back((*chunk_or)->toString());
    }
    done = true;
    co_return absl::OkStatus();
  }(buf, received, completed);

  auto handle = Coroutine::launch(
      std::move(task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);

  // Initially coroutine is suspended waiting for chunk
  EXPECT_FALSE(completed);
  EXPECT_TRUE(received.empty());

  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("hello "), false);
  EXPECT_EQ(received, (std::vector<std::string>{"hello "}));
  EXPECT_FALSE(completed);

  FieldStreamPeer::pushChunk(buf, Buffer::OwnedImpl("world!"), true);
  EXPECT_EQ(received, (std::vector<std::string>{"hello ", "world!"}));
  EXPECT_TRUE(completed);
}

TEST_F(FieldStreamTest, QueueSequenceRecv) {
  auto q1 = std::make_shared<ByteQueue>();
  auto q2 = std::make_shared<ByteQueue>();

  std::deque<ByteQueuePtr> queues = {q1, q2};
  FieldStream buf("/content", std::move(queues));

  // Push into q1 (prefix)
  q1->tryPush(Buffer::OwnedImpl("prefix_"));
  q1->close();

  // Push into q2 (remainder)
  q2->tryPush(Buffer::OwnedImpl("remainder_data"));
  q2->close();

  bool completed = false;
  std::string full_text;

  auto recv_task = [](FieldStream b, std::string& out,
                      bool& done) -> Coroutine::Task<absl::Status> {
    while (true) {
      auto chunk_or = co_await b.recv();
      if (!chunk_or.ok()) {
        co_return chunk_or.status();
      }
      if (!chunk_or->has_value()) {
        break;
      }
      out += (*chunk_or)->toString();
    }
    done = true;
    co_return absl::OkStatus();
  }(buf, full_text, completed);

  auto h = Coroutine::launch(
      std::move(recv_task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);

  EXPECT_TRUE(completed);
  EXPECT_EQ(full_text, "prefix_remainder_data");
}

TEST_F(FieldStreamTest, ForwardPushesToOutputQueue) {
  auto out_q = std::make_shared<ByteQueue>();
  FieldStream buf("/content", std::deque<ByteQueuePtr>{}, out_q);

  bool completed = false;
  std::string full_text;

  auto recv_task = [](ByteQueuePtr q, std::string& out,
                      bool& done) -> Coroutine::Task<absl::Status> {
    while (true) {
      auto chunk_or = co_await q->pop();
      if (!chunk_or.ok()) {
        co_return chunk_or.status();
      }
      if (!chunk_or->has_value()) {
        break;
      }
      out += (*chunk_or)->toString();
    }
    done = true;
    co_return absl::OkStatus();
  }(out_q, full_text, completed);

  auto fwd_task = [](FieldStream b) -> Coroutine::Task<absl::Status> {
    auto s1 = co_await b.forward(Buffer::OwnedImpl("first "), false);
    EXPECT_TRUE(s1.ok());
    auto s2 = co_await b.forward(Buffer::OwnedImpl("second"), true);
    EXPECT_TRUE(s2.ok());
    co_return absl::OkStatus();
  }(buf);

  auto h1 = Coroutine::launch(
      std::move(recv_task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);
  auto h2 = Coroutine::launch(
      std::move(fwd_task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);

  EXPECT_TRUE(completed);
  EXPECT_EQ(full_text, "first second");
}

TEST_F(FieldStreamTest, CloseSignalsEOF) {
  FieldStream buf("/close_test");

  std::optional<absl::StatusOr<std::optional<Buffer::OwnedImpl>>> received_result;
  auto task = [](FieldStream& b,
                 std::optional<absl::StatusOr<std::optional<Buffer::OwnedImpl>>>& out)
      -> Coroutine::Task<absl::Status> {
    out = co_await b.recv();
    co_return absl::OkStatus();
  }(buf, received_result);

  auto handle = Coroutine::launch(
      std::move(task), executor_, [](absl::Status) {}, Coroutine::StartMode::Inline);

  EXPECT_FALSE(received_result.has_value());
  FieldStreamPeer::close(buf);
  EXPECT_TRUE(received_result.has_value());
  EXPECT_TRUE(received_result->ok());
  EXPECT_FALSE((*received_result)->has_value());
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
