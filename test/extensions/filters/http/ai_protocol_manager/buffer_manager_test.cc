#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain_writer.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/stream_payload.h"

#include "test/extensions/filters/http/ai_protocol_manager/fake_bridge.h"
#include "test/mocks/event/mocks.h"
#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// An ExternalBuffer that completes both write and read asynchronously, via
// dispatcher.post() -- modelling a network/disk-backed store. Used to exercise
// the BufferManager's asynchronous (non-re-entrant) replay path, where each read
// completion runs on its own event-loop iteration.
class PostingExternalBuffer : public ExternalBuffer {
public:
  explicit PostingExternalBuffer(Event::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
  ~PostingExternalBuffer() override { *alive_ = false; }

  void write(Buffer::InstancePtr data, bool end_stream, WriteCallback cb) override {
    // Enforce the single-writer contract: the manager must never issue a write
    // while another is outstanding.
    EXPECT_FALSE(write_active_);
    write_active_ = true;
    ++write_calls_;
    write_sizes_.push_back(data->length());
    write_end_streams_.push_back(end_stream);
    dispatcher_.post([this, alive = alive_, data = std::move(data), cb = std::move(cb)]() mutable {
      if (!*alive) {
        return;
      }
      data_.move(*data);
      write_active_ = false;
      cb(ExternalBufferStatus::Ok);
    });
  }
  void read(uint64_t offset, uint64_t length, ReadCallback cb) override {
    auto out = std::make_unique<Buffer::OwnedImpl>();
    if (length > 0) {
      auto slice = std::make_unique<uint8_t[]>(length);
      data_.copyOut(offset, length, slice.get());
      out->add(slice.get(), length);
    }
    dispatcher_.post([alive = alive_, out = std::move(out), cb = std::move(cb)]() mutable {
      if (!*alive) {
        return;
      }
      cb(ExternalBufferStatus::Ok, std::move(out));
    });
  }
  uint64_t length() const override { return data_.length(); }

  // Number of write() calls and the byte length each carried, so tests can assert
  // that the manager serializes and coalesces queued writes.
  int write_calls_{0};
  std::vector<uint64_t> write_sizes_;
  std::vector<bool> write_end_streams_;

private:
  Event::Dispatcher& dispatcher_;
  Buffer::OwnedImpl data_;
  // True while a write's posted completion has not yet run; used to verify the
  // single-writer contract.
  bool write_active_{false};
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

class PostingExternalBufferFactory : public ExternalBufferFactory {
public:
  using ExternalBufferFactory::createBuffer;
  ExternalBufferPtr createBuffer(Event::Dispatcher& dispatcher) override {
    auto buffer = std::make_unique<PostingExternalBuffer>(dispatcher);
    last_ = buffer.get();
    ++create_calls_;
    return buffer;
  }

  // The most recently created buffer (owned by the BufferManager under test).
  PostingExternalBuffer* last_{nullptr};
  // Counted so a test can assert that a payload within the in-memory limit creates no store.
  int create_calls_{0};
};

// An ExternalBuffer that reports an I/O error from its write or read completion,
// to exercise the BufferManager's unrecoverable-error path. The write completion
// is posted (so the test drives it via the event loop); the read completes
// synchronously, like the in-memory store.
class FailingExternalBuffer : public ExternalBuffer {
public:
  enum class FailMode { Write, Read };
  FailingExternalBuffer(Event::Dispatcher& dispatcher, FailMode mode)
      : dispatcher_(dispatcher), mode_(mode) {}
  ~FailingExternalBuffer() override { *alive_ = false; }

  void write(Buffer::InstancePtr data, bool /*end_stream*/, WriteCallback cb) override {
    dispatcher_.post([this, alive = alive_, data = std::move(data), cb = std::move(cb)]() mutable {
      if (!*alive) {
        return;
      }
      if (mode_ == FailMode::Write) {
        cb(ExternalBufferStatus::Error);
        return;
      }
      // A read-failure run still needs the bytes to become durable so replay can
      // begin and reach the failing read.
      data_.move(*data);
      cb(ExternalBufferStatus::Ok);
    });
  }
  void read(uint64_t offset, uint64_t length, ReadCallback cb) override {
    if (mode_ == FailMode::Read) {
      // On error `data` is nullptr (see external_buffer.h).
      cb(ExternalBufferStatus::Error, nullptr);
      return;
    }
    auto out = std::make_unique<Buffer::OwnedImpl>();
    if (length > 0) {
      auto slice = std::make_unique<uint8_t[]>(length);
      data_.copyOut(offset, length, slice.get());
      out->add(slice.get(), length);
    }
    cb(ExternalBufferStatus::Ok, std::move(out));
  }
  uint64_t length() const override { return data_.length(); }

private:
  Event::Dispatcher& dispatcher_;
  FailMode mode_;
  Buffer::OwnedImpl data_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

class FailingExternalBufferFactory : public ExternalBufferFactory {
public:
  using ExternalBufferFactory::createBuffer;
  explicit FailingExternalBufferFactory(FailingExternalBuffer::FailMode mode) : mode_(mode) {}
  ExternalBufferPtr createBuffer(Event::Dispatcher& dispatcher) override {
    return std::make_unique<FailingExternalBuffer>(dispatcher, mode_);
  }
  FailingExternalBuffer::FailMode mode_;
};

class TrackedSchedulableCallback : public NiceMock<Event::MockSchedulableCallback> {
public:
  TrackedSchedulableCallback(Event::MockDispatcher* dispatcher, std::function<void()> cb,
                             NiceMock<Event::MockSchedulableCallback>** slot)
      : NiceMock<Event::MockSchedulableCallback>(dispatcher, std::move(cb)), slot_(slot) {}

  ~TrackedSchedulableCallback() override {
    *alive_ = false;
    if (slot_ != nullptr && *slot_ == this) {
      *slot_ = nullptr;
    }
  }

  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

private:
  NiceMock<Event::MockSchedulableCallback>** slot_{nullptr};
};

class BufferManagerTest : public testing::Test {
public:
  BufferManagerTest() {
    // The in-memory buffer delivers write completions via dispatcher.post().
    // Capture those callbacks so the test can run the event loop deterministically.
    ON_CALL(dispatcher_, post(testing::_)).WillByDefault([this](Event::PostCb cb) {
      posted_.push_back(std::move(cb));
    });
    // Current-iteration schedulable callbacks enqueue onto `posted_` (while keeping `enabled()`
    // observable prior to `drain()`), whereas next-iteration budget yields remain pending until
    // the test explicitly advances the iteration via `replay_cb_->invokeCallback()`.
    ON_CALL(dispatcher_, createSchedulableCallback_(testing::_))
        .WillByDefault([this](std::function<void()> cb) -> Event::SchedulableCallback* {
          auto* scb = new TrackedSchedulableCallback(&dispatcher_, cb, &replay_cb_);
          replay_cb_ = scb;
          ON_CALL(*scb, scheduleCallbackCurrentIteration())
              .WillByDefault([this, scb, alive = scb->alive_, cb]() {
                scb->enabled_ = true;
                posted_.push_back([scb, alive, cb]() {
                  if (*alive && scb->enabled_) {
                    scb->enabled_ = false;
                    cb();
                  }
                });
              });
          return scb;
        });
    resetManager(factory_);
  }

  ~BufferManagerTest() override {
    cancelActiveTask();
    if (manager_ != nullptr) {
      manager_->onDestroy();
    }
    drain();
  }

  // Builds a BufferManager backed by `factory`, wiring a fresh FakeBridge. bridge_ points
  // at the new one; replay_cb_ is filled in if and when the writer creates its schedule callback.
  BufferManagerPtr makeManager(ExternalBufferFactory& factory) {
    bridges_.push_back(std::make_unique<FakeBridge>(dispatcher_, bridge_buffer_limit_));
    bridge_ = bridges_.back().get();
    replay_cb_ = nullptr;
    return std::make_unique<BufferManager>(manager_config_, factory, *bridge_);
  }

  // Swaps in a manager and writer backed by `factory`, cancelling active coroutines and detaching
  // the outgoing manager first.
  void resetManager(ExternalBufferFactory& factory) {
    cancelActiveTask();
    if (manager_ != nullptr) {
      manager_->onDestroy();
    }
    drain();
    exported_reader_.reset();
    manager_ = makeManager(factory);
    writer_ = std::make_unique<ChainWriter>(*bridge_);
  }

  PayloadReader& exportedReader() {
    if (exported_reader_ == nullptr) {
      exported_reader_ = BufferManager::exportPayloadReader(std::move(manager_));
    }
    return *exported_reader_;
  }

  void cancelActiveTask() {
    if (std::optional<Coroutine::DetachedHandle> handle = std::move(active_handle_);
        handle.has_value()) {
      handle->cancel();
    }
  }

  void launchStream(ChainWriter& writer, PayloadReader& reader, uint64_t offset, uint64_t length,
                    absl::AnyInvocable<void(absl::Status)> done) {
    last_status_ = absl::UnknownError("pending");
    active_handle_ = Coroutine::launch(
        streamPayload(reader, offset, length, writer), executor_,
        [this, done = std::move(done)](absl::Status status) mutable {
          last_status_ = status;
          if (!status.ok()) {
            return;
          }
          done(std::move(status));
        },
        Coroutine::StartMode::Inline);
  }

  void launchInject(ChainWriter& writer, Buffer::Instance& data,
                    absl::AnyInvocable<void(absl::Status)> done) {
    last_status_ = absl::UnknownError("pending");
    active_handle_ = Coroutine::launch(
        writer.inject(data), executor_,
        [this, done = std::move(done)](absl::Status status) mutable {
          last_status_ = status;
          if (!status.ok()) {
            return;
          }
          done(std::move(status));
        },
        Coroutine::StartMode::Inline);
  }

  // Streams the whole offloaded payload, recording completion in replay_done_.
  void replayAll() {
    PayloadReader& reader = exportedReader();
    launchStream(*writer_, reader, 0, reader.length(), [this](absl::Status status) {
      if (status.ok()) {
        replay_done_ = true;
      }
    });
  }

  // Run all posted callbacks, including ones enqueued while draining.
  void drain() {
    while (!posted_.empty()) {
      Event::PostCb cb = std::move(posted_.front());
      posted_.pop_front();
      cb();
    }
  }

  // Runs exactly one posted callback (FIFO), leaving the rest queued -- lets a test
  // pause with a write or read still in flight.
  void runOnePosted() {
    ASSERT_FALSE(posted_.empty());
    Event::PostCb cb = std::move(posted_.front());
    posted_.pop_front();
    cb();
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  std::deque<Event::PostCb> posted_;
  // Offload from the first byte unless a test opts into the in-memory tier (before resetManager()).
  BufferManager::Config manager_config_{};
  // The bridge samples this at construction, so a test must set it before resetManager().
  uint32_t bridge_buffer_limit_{1024 * 1024};
  InMemoryExternalBufferFactory factory_;
  // Declared before manager_ so it outlives the manager that references it.
  PostingExternalBufferFactory posting_factory_;
  FailingExternalBufferFactory write_failing_factory_{FailingExternalBuffer::FailMode::Write};
  FailingExternalBufferFactory read_failing_factory_{FailingExternalBuffer::FailMode::Read};
  // Bridges outlive the managers that reference them, so they are kept here rather than owned by
  // the manager.
  std::vector<std::unique_ptr<FakeBridge>> bridges_;
  FakeBridge* bridge_{nullptr};
  // Owned by writer_, created on its first scheduled yield; fire invokeCallback() to simulate the
  // next event-loop iteration resuming replay after a per-iteration budget yield, starting a
  // replay that was deferred because the offload was already durable, or resuming
  // replay after chain back-pressure clears (deferred out of the watermark
  // callback).
  NiceMock<Event::MockSchedulableCallback>* replay_cb_{nullptr};
  bool replay_done_{false};
  absl::Status last_status_{absl::UnknownError("pending")};
  std::shared_ptr<Coroutine::DispatcherExecutor> executor_{
      std::make_shared<Coroutine::DispatcherExecutor>(dispatcher_)};
  std::optional<Coroutine::DetachedHandle> active_handle_;
  BufferManagerPtr manager_;
  PayloadReaderSharedPtr exported_reader_;
  ChainWriterPtr writer_;
};

// The body is offloaded chunk-by-chunk and replayed verbatim once the caller
// triggers replay; completion is reported through the done callback.
TEST_F(BufferManagerTest, OffloadsAndReplaysBody) {
  Buffer::OwnedImpl chunk1("{\"messages\":");
  manager_->onData(chunk1);
  Buffer::OwnedImpl chunk2("[\"hi\"]}");
  manager_->onData(chunk2);
  manager_->endStream();
  replayAll();

  // Nothing has been replayed yet: writes and replay are all asynchronous.
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);

  drain();

  EXPECT_GE(bridge_->inject_calls_, 1);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "{\"messages\":[\"hi\"]}");
}

// A body that arrives in a single frame is still round-tripped.
TEST_F(BufferManagerTest, SingleFrameBody) {
  Buffer::OwnedImpl body("{}");
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  drain();

  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "{}");
}

// An empty body has nothing to inject: a zero-length streamPayload resolves immediately without
// suspending or touching the bridge.
TEST_F(BufferManagerTest, EmptyBody) {
  Buffer::OwnedImpl empty;
  manager_->onData(empty);
  manager_->endStream();
  replayAll();

  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->inject_calls_, 0);
}

// A payload larger than the replay chunk size is streamed back in multiple
// bounded frames and reassembles to the original bytes.
TEST_F(BufferManagerTest, LargePayloadReplayedInChunks) {
  const std::string big(200 * 1024, 'x'); // > ReadChunkSize (64KiB)
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  drain();

  EXPECT_GT(bridge_->inject_calls_, 1);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.length(), big.size());
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// A payload larger than the per-iteration replay budget (ReplayChunksPerIteration
// chunks of ReadChunkSize) is not replayed all at once: replay injects one budget
// of chunks, yields to the event loop, and resumes via the scheduled continuation.
// This keeps a large replay from monopolizing the worker thread.
TEST_F(BufferManagerTest, ReplayYieldsAcrossIterationsForLargePayload) {
  // 10 chunks total; the budget is 8, so replay should stop after 8 and resume
  // for the remaining 2 on the next iteration.
  const uint64_t chunk = 64 * 1024;
  const std::string big(10 * chunk, 'x');
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // First iteration: exactly the budget of chunks is injected, then replay yields
  // (the continuation is scheduled, not posted) without finishing.
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 8);
  EXPECT_EQ(bridge_->injected_.length(), 8 * chunk);
  EXPECT_FALSE(replay_done_);
  EXPECT_TRUE(replay_cb_->enabled());

  // The scheduled continuation (next event-loop iteration) resumes replay to
  // completion with the original bytes intact.
  replay_cb_->invokeCallback();
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 10);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// An asynchronous store completes each chunk read via dispatcher.post() and streams all chunks
// across the per-iteration budget boundary cleanly.
TEST_F(BufferManagerTest, AsyncStoreReplaysOneChunkPerCallWithoutYield) {
  resetManager(posting_factory_);

  const std::string big(10 * 64 * 1024, 'x'); // 10 chunks > InjectChunksPerIteration (8).
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  drain();

  ASSERT_NE(replay_cb_, nullptr);
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();

  EXPECT_EQ(bridge_->inject_calls_, 10);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// Replay is caller-triggered: the body can be fully offloaded and durable before
// the caller decides to replay. replay() then starts playback on a fresh
// event-loop iteration, decoupling offload from playback.
TEST_F(BufferManagerTest, ReplayDeferredUntilCallerRequests) {
  Buffer::OwnedImpl body("{\"messages\":[\"hi\"]}");
  manager_->onData(body);
  manager_->endStream();

  // The write completes, but with no replay() request nothing is injected.
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 0);

  // The caller decides to proceed. Replay yields via Coroutine::yield() (the
  // caller may be inside a data callback), so nothing is injected synchronously.
  replayAll();
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(posted_.empty());

  drain();
  EXPECT_GE(bridge_->inject_calls_, 1);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "{\"messages\":[\"hi\"]}");
}

// The caller can stream the payload back as independent sub-ranges, chaining the
// next from each range's done callback. The ranges reassemble to the full payload.
TEST_F(BufferManagerTest, ReplaysSubRangesInSequence) {
  Buffer::OwnedImpl body("HELLOWORLD");
  manager_->onData(body);
  manager_->endStream();
  drain(); // Make the payload durable; replay not yet requested.
  EXPECT_EQ(bridge_->inject_calls_, 0);

  bool second_done = false;
  // Replay the first half, then chain the second half from its done callback.
  launchStream(*writer_, exportedReader(), 0, 5, [&](absl::Status status) {
    ASSERT_OK(status);
    EXPECT_EQ(bridge_->injected_.toString(), "HELLO");
    launchStream(*writer_, exportedReader(), 5, 5, [&second_done](absl::Status status2) {
      ASSERT_OK(status2);
      second_done = true;
    });
  });

  // First range was deferred (offload already durable); once deferred_initial_ is satisfied on the
  // writer, the chained second range fits within the same 8-chunk iteration budget.
  EXPECT_EQ(bridge_->inject_calls_, 0);
  drain();
  EXPECT_TRUE(second_done);
  EXPECT_EQ(bridge_->injected_.toString(), "HELLOWORLD");
}

// Cancelling the coroutine context and dropping the pending reader mid-flight stops pending replay.
TEST_F(BufferManagerTest, DestroyBeforeReplay) {
  Buffer::OwnedImpl body("payload");
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  cancelActiveTask();
  exported_reader_.reset();
  drain();

  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);
}

// A synchronous store completes its final replay read on-stack, so the stream completion callback
// may end the stream and detach the bridge on-stack -- exactly what the real filter does when the
// terminal end-of-stream injection triggers teardown.
TEST_F(BufferManagerTest, TerminalReplayDoneDetachesManagerSynchronously) {
  Buffer::OwnedImpl body("payload");
  manager_->onData(body);
  manager_->endStream();

  std::string injected;
  bool done_ran = false;
  launchStream(*writer_, exportedReader(), 0, exportedReader().length(), [&](absl::Status status) {
    ASSERT_OK(status);
    done_ran = true;
    injected = bridge_->injected_.toString();
    bridge_->detachFromFilterChain();
  });

  drain();
  EXPECT_TRUE(done_ran);
  EXPECT_EQ(injected, "payload");
}

// Writers share one bridge and take its write-resume slot only while paused, so a second writer on
// the same bridge can stream after the first is done.
TEST_F(BufferManagerTest, ManagersTakeTheBridgeReplaySlotInTurn) {
  Buffer::OwnedImpl body("first");
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  drain();
  ASSERT_TRUE(replay_done_);

  auto second = std::make_unique<BufferManager>(manager_config_, factory_, *bridge_);
  ChainWriter second_writer(*bridge_);
  Buffer::OwnedImpl more("second");
  second->onData(more);
  second->endStream();
  PayloadReaderSharedPtr second_reader = BufferManager::exportPayloadReader(std::move(second));
  bool second_done = false;
  launchStream(second_writer, *second_reader, 0, second_reader->length(),
               [&second_done](absl::Status status) { second_done = status.ok(); });
  drain();
  EXPECT_TRUE(second_done);
  EXPECT_EQ(bridge_->injected_.toString(), "firstsecond");
}

// With no body offloaded the manager reports empty(), so the caller knows there is
// nothing to replay (a trailer-only request lets its trailers flow without calling
// endStream()/replay()).
TEST_F(BufferManagerTest, EmptyWhenNoDataOffloaded) {
  EXPECT_TRUE(manager_->empty());
  Buffer::OwnedImpl body("x");
  manager_->onData(body);
  EXPECT_FALSE(manager_->empty());
}

// length() reports the total offloaded so far, including not-yet-durable bytes.
TEST_F(BufferManagerTest, LengthCountsNotYetDurableBytes) {
  Buffer::OwnedImpl chunk1("abc");
  manager_->onData(chunk1);
  Buffer::OwnedImpl chunk2("defg");
  manager_->onData(chunk2);
  manager_->endStream();
  // The writes are still in flight (posted), but length() already reflects them.
  EXPECT_EQ(manager_->length(), 7);
  drain();
  EXPECT_EQ(manager_->length(), 7);
}

// Not-yet-durable bytes crossing the buffer limit pause the data source via the
// bridge; once the write completes and they drain, the source resumes.
TEST_F(BufferManagerTest, IngestBackpressureDrivesSourceFlowControl) {
  // Small limit so a single frame crosses the high watermark (high=100, low=50).
  bridge_buffer_limit_ = 100;
  resetManager(factory_);

  // The frame exceeds the high watermark. The in-memory write completion is
  // posted, so its bytes stay not-yet-durable until the event loop runs: the
  // source is paused immediately.
  Buffer::OwnedImpl body(std::string(150, 'x'));
  manager_->onData(body);
  EXPECT_EQ(bridge_->pause_source_calls_, 1);
  EXPECT_EQ(bridge_->resume_source_calls_, 0);

  // Draining makes the write durable; not-yet-durable bytes fall to zero (<= low)
  // and the source resumes.
  drain();
  EXPECT_EQ(bridge_->resume_source_calls_, 1);
}

// Sub-threshold frames are batched: each is held in the queue rather than written
// individually, and they coalesce into a single write flushed at endStream().
TEST_F(BufferManagerTest, BatchesSmallFramesUntilEndStream) {
  resetManager(posting_factory_);

  Buffer::OwnedImpl f1("aaa");
  Buffer::OwnedImpl f2("bbb");
  manager_->onData(f1);
  manager_->onData(f2);

  // The buffer is created lazily on the first onData(); neither small frame has
  // triggered a write -- they wait in the queue.
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->write_calls_, 0);

  Buffer::OwnedImpl f3("cc");
  manager_->onData(f3);
  manager_->endStream();

  // endStream() flushes the batched backlog as a single write.
  EXPECT_EQ(buffer->write_calls_, 1);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(8)); // "aaa"+"bbb"+"cc".
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(true));

  replayAll();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "aaabbbcc");
}

// A frame at the flush threshold is written immediately; frames that arrive while
// that write is in flight queue and coalesce into a single follow-up write. The
// manager issues exactly one write at a time (asserted in the fake) and replays
// the reassembled payload verbatim.
TEST_F(BufferManagerTest, SerializesAndCoalescesQueuedWrites) {
  resetManager(posting_factory_);

  // Each frame is exactly the flush threshold (WriteFlushThreshold == 64KiB), so
  // the first flushes at once.
  const uint64_t threshold = 64 * 1024;
  const std::string chunk(threshold, 'x');
  Buffer::OwnedImpl f1(chunk);
  Buffer::OwnedImpl f2(chunk);
  Buffer::OwnedImpl f3(chunk);
  manager_->onData(f1);
  manager_->onData(f2);
  manager_->onData(f3);
  manager_->endStream();

  // Only the first frame has been written; f2 and f3 wait behind the in-flight write.
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->write_calls_, 1);

  replayAll();
  drain();

  // The queued backlog coalesced into one follow-up write -- two writes total,
  // never overlapping (asserted in the fake) -- and the payload round-trips.
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(threshold, 2 * threshold));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(false, true));
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), chunk + chunk + chunk);
}

// The terminal write is the store's commit signal. When the last data write has
// already drained the backlog before endStream() arrives, the manager still
// issues one zero-length write carrying end_stream, and replay waits for it.
TEST_F(BufferManagerTest, EndStreamAfterDrainedBacklogIssuesTerminalWrite) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  Buffer::OwnedImpl frame(chunk);
  manager_->onData(frame);
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->write_calls_, 1);
  drain();

  manager_->endStream();
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(chunk.size(), 0));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(false, true));

  replayAll();
  drain();
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), chunk);
}

// endStream() while a data write is in flight is carried by one zero-length
// follow-up write.
TEST_F(BufferManagerTest, EndStreamDuringInFlightWriteIssuesOneTerminalWrite) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  Buffer::OwnedImpl frame(chunk);
  manager_->onData(frame);
  manager_->endStream();
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->write_calls_, 1);

  replayAll();
  drain();
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(chunk.size(), 0));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(false, true));
  EXPECT_TRUE(replay_done_);
}

// A last frame passed with end_stream is written once, carrying end_stream.
TEST_F(BufferManagerTest, FinalFrameWithEndStreamIsWrittenOnce) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  Buffer::OwnedImpl frame(chunk);
  manager_->onData(frame, /*end_stream=*/true);
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  manager_->endStream();
  EXPECT_EQ(buffer->write_calls_, 1);

  replayAll();
  drain();
  EXPECT_EQ(buffer->write_calls_, 1);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(chunk.size()));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(true));
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), chunk);
}

// A last frame passed with end_stream during an in-flight write goes down in the
// follow-up write, which carries end_stream.
TEST_F(BufferManagerTest, FinalFrameWithEndStreamDuringInFlightWriteFoldsIntoFollowUp) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  const std::string tail(16, 'y');
  Buffer::OwnedImpl frame(chunk);
  Buffer::OwnedImpl last_frame(tail);
  manager_->onData(frame);
  manager_->onData(last_frame, /*end_stream=*/true);
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->write_calls_, 1);

  replayAll();
  drain();
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(chunk.size(), tail.size()));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(false, true));
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), chunk + tail);
}

// An empty end_stream frame after the backlog has drained is a zero-length
// terminal write.
TEST_F(BufferManagerTest, EmptyFinalFrameAfterDrainedBacklogIssuesTerminalWrite) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  Buffer::OwnedImpl frame(chunk);
  manager_->onData(frame);
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);
  drain();

  Buffer::OwnedImpl empty;
  manager_->onData(empty, /*end_stream=*/true);
  EXPECT_EQ(buffer->write_calls_, 2);
  EXPECT_THAT(buffer->write_sizes_, testing::ElementsAre(chunk.size(), 0));
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(false, true));

  replayAll();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), chunk);
}

// A small body passed with end_stream stays in memory and creates no store.
TEST_F(BufferManagerTest, FinalFrameWithEndStreamWithinInMemoryTierCreatesNoStore) {
  manager_config_.max_in_memory_bytes = 1024;
  resetManager(posting_factory_);

  Buffer::OwnedImpl body("{}");
  manager_->onData(body, /*end_stream=*/true);
  manager_->endStream();
  EXPECT_EQ(posting_factory_.create_calls_, 0);

  replayAll();
  EXPECT_EQ(bridge_->inject_calls_, 0);
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "{}");
}

// Data after end of stream is reported and dropped.
TEST_F(BufferManagerTest, DataAfterEndStreamIsReportedAndDropped) {
  resetManager(posting_factory_);

  const std::string chunk(64 * 1024, 'x');
  Buffer::OwnedImpl frame(chunk);
  manager_->onData(frame, /*end_stream=*/true);
  PostingExternalBuffer* buffer = posting_factory_.last_;
  ASSERT_NE(buffer, nullptr);

  Buffer::OwnedImpl late("late");
  EXPECT_ENVOY_BUG(manager_->onData(late), "data after end of stream");
  drain();
  EXPECT_THAT(buffer->write_end_streams_, testing::ElementsAre(true));
}

// When the chain we replay into is backed up before replay starts, no data is
// injected until the back-pressure is released.
TEST_F(BufferManagerTest, ReplayPausesUnderBackPressure) {
  const std::string big(200 * 1024, 'x'); // > ReadChunkSize, multiple chunks.
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // The chain signals back-pressure before replay begins.
  ASSERT_TRUE(bridge_->subscribed_);
  bridge_->raiseWriteWatermark();

  // Draining completes the write and starts replay, but replay is paused: no
  // chunk is read or injected while the high watermark is held.
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);

  // Releasing back-pressure schedules the resume off the watermark callback stack
  // (deferred so we never read/inject reentrantly from within it); firing the
  // continuation runs replay to completion.
  bridge_->lowerWriteWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.length(), big.size());
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// Back-pressure arising mid-replay (the chain fills as we inject) halts the
// synchronous read loop, and replay resumes when it clears.
TEST_F(BufferManagerTest, ReplayResumesMidStream) {
  const std::string big(200 * 1024, 'x'); // 4 chunks of 64KiB + remainder.
  // The chain raises its replay high watermark right after the first injected
  // chunk; the loop must then stop until it is released.
  bridge_->raise_write_watermark_at_inject_ = 1;
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // The write completes, replay injects one chunk, then pauses on the watermark
  // raised during that inject.
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 1);
  EXPECT_FALSE(replay_done_);
  EXPECT_LT(bridge_->injected_.length(), big.size());

  // Release back-pressure; the resume is scheduled off the watermark callback
  // stack (deferred to avoid reentrant read/inject) and the continuation runs
  // replay to completion.
  ASSERT_TRUE(bridge_->subscribed_);
  bridge_->lowerWriteWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// High watermark callbacks can nest (stream + connection); replay resumes only
// after a matching number of low-watermark callbacks.
TEST_F(BufferManagerTest, NestedWatermarksRequireBalancedRelease) {
  const std::string big(200 * 1024, 'x');
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  ASSERT_TRUE(bridge_->subscribed_);
  bridge_->raiseWriteWatermark();
  bridge_->raiseWriteWatermark();
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 0);

  // One release is not enough to resume.
  bridge_->lowerWriteWatermark();
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);

  // Balanced release schedules the resume off the watermark callback stack; the
  // continuation runs replay to completion.
  bridge_->lowerWriteWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// A write that fails fails the stream: the manager surfaces the unrecoverable
// error through the bridge and injects nothing. A pending replay never starts.
TEST_F(BufferManagerTest, WriteErrorFailsStream) {
  resetManager(write_failing_factory_);

  Buffer::OwnedImpl body("payload");
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // The failing completion is posted; nothing has happened yet.
  EXPECT_EQ(bridge_->error_calls_, 0);

  drain();

  EXPECT_EQ(bridge_->error_calls_, 1);
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);
}

// A read that fails during streamPayload returns DataLossError without injecting the failed chunk.
TEST_F(BufferManagerTest, ReadErrorFailsStream) {
  resetManager(read_failing_factory_);

  Buffer::OwnedImpl body("payload");
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // Draining makes the write durable and starts streamPayload, whose first read fails.
  drain();

  EXPECT_EQ(last_status_.code(), absl::StatusCode::kDataLoss);
  EXPECT_TRUE(exportedReader().hasFailed());
  EXPECT_EQ(bridge_->inject_calls_, 0);
  EXPECT_FALSE(replay_done_);
}

// An external-buffer error delivered after onDestroy() is inert: the completion
// handlers bail out and never touch the (detached) bridge.
TEST_F(BufferManagerTest, ErrorAfterDestroyIsInert) {
  resetManager(write_failing_factory_);

  Buffer::OwnedImpl body("payload");
  manager_->onData(body);
  manager_->endStream();
  replayAll();
  cancelActiveTask();
  exported_reader_.reset();

  // The posted write completion (whether it carried Ok or Error) is ignored once the self-owned
  // manager sees its exported reader expired; no error reaches the bridge.
  drain();
  EXPECT_EQ(bridge_->error_calls_, 0);
}

// With an asynchronous store, a replay resumed by a watermark drain while a read is
// still in flight must not issue an overlapping read -- it waits for the in-flight
// read to complete.
TEST_F(BufferManagerTest, ResumeSkipsReadWhileOneInFlight) {
  resetManager(posting_factory_);

  const std::string big(200 * 1024, 'x'); // Multiple chunks.
  Buffer::OwnedImpl body(big);
  manager_->onData(body);
  manager_->endStream();
  replayAll();

  // Run the data write completion, then the zero-length terminal write it
  // triggers: replay starts after the second and posts its first read (now in
  // flight, not yet completed).
  runOnePosted();
  runOnePosted();

  // A watermark cycle while the replay coroutine is parked in co_await reader.readChunk() (rather
  // than parked on watermark back-pressure) does not schedule a spurious continuation or issue an
  // overlapping read.
  ASSERT_TRUE(bridge_->subscribed_);
  bridge_->raiseWriteWatermark();
  bridge_->lowerWriteWatermark();
  EXPECT_EQ(replay_cb_, nullptr);

  // Draining the in-flight read (and its successors) still completes the replay
  // with the payload intact -- no read was dropped or duplicated.
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// Inject injects small buffer data into the filter chain verbatim and triggers done
// callback.
TEST_F(BufferManagerTest, InjectsCallerDataVerbatim) {
  Buffer::OwnedImpl body("{\"messages\":[\"hello\"]}");
  manager_->onData(body);
  manager_->endStream();
  drain();

  Buffer::OwnedImpl inject_data("{\"modified\":true}");
  bool inject_done = false;
  launchInject(*writer_, inject_data, [&inject_done](absl::Status status) {
    ASSERT_OK(status);
    inject_done = true;
  });

  EXPECT_EQ(bridge_->inject_calls_, 0);
  drain();

  EXPECT_TRUE(inject_done);
  EXPECT_EQ(bridge_->injected_.toString(), "{\"modified\":true}");
}

// Inject pauses when high watermark is hit and resumes when low watermark drains.
TEST_F(BufferManagerTest, InjectRespectsWatermarkBackpressure) {
  Buffer::OwnedImpl body("initial");
  manager_->onData(body);
  manager_->endStream();
  drain();

  // Set bridge to raise high watermark on the first injected chunk.
  bridge_->raise_write_watermark_at_inject_ = 1;

  const std::string big_payload(200 * 1024, 'z'); // Multiple 64KB chunks
  Buffer::OwnedImpl inject_data(big_payload);
  bool inject_done = false;
  launchInject(*writer_, inject_data, [&inject_done](absl::Status status) {
    ASSERT_OK(status);
    inject_done = true;
  });

  drain();

  // The first chunk injected and triggered high watermark, pausing injection.
  EXPECT_FALSE(inject_done);
  EXPECT_EQ(bridge_->injected_.length(), 64 * 1024);

  // Now clear the watermark: low watermark schedules continuation to resume draining.
  ASSERT_TRUE(bridge_->subscribed_);
  bridge_->lowerWriteWatermark();
  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();

  EXPECT_TRUE(inject_done);
  EXPECT_EQ(bridge_->injected_.toString(), big_payload);
}

// Inject larger than the bridge's inject budget stops mid-drain, yields to the event
// loop, and finishes on the continuation that refills the budget.
TEST_F(BufferManagerTest, InjectYieldsWhenTheBudgetIsSpent) {
  Buffer::OwnedImpl body("initial");
  manager_->onData(body);
  manager_->endStream();
  drain();

  const std::string big_payload(10 * 64 * 1024, 'z'); // 10 chunks, over the 8-chunk budget.
  Buffer::OwnedImpl inject_data(big_payload);
  bool inject_done = false;
  launchInject(*writer_, inject_data, [&inject_done](absl::Status status) {
    ASSERT_OK(status);
    inject_done = true;
  });

  drain();

  // The budget stops the drain a chunk short of the payload; the rest waits for the next pass.
  EXPECT_FALSE(inject_done);
  EXPECT_EQ(bridge_->injected_.length(), 8 * 64 * 1024);

  ASSERT_TRUE(replay_cb_->enabled());
  replay_cb_->invokeCallback();
  EXPECT_TRUE(inject_done);
  EXPECT_EQ(bridge_->injected_.toString(), big_payload);
}

// A payload that stays within the in-memory limit never creates a store: it is replayed straight
// out of the ingest queue, with no write and no read.
TEST_F(BufferManagerTest, PayloadUnderTheInMemoryLimitSkipsStorage) {
  manager_config_.max_in_memory_bytes = 1024;
  resetManager(posting_factory_);

  Buffer::OwnedImpl chunk1("{\"messages\":");
  manager_->onData(chunk1);
  Buffer::OwnedImpl chunk2("[\"hello\"]}");
  manager_->onData(chunk2);
  manager_->endStream();
  EXPECT_EQ(posting_factory_.create_calls_, 0);
  EXPECT_EQ(manager_->length(), 22);
  EXPECT_FALSE(manager_->empty());

  replayAll();
  EXPECT_EQ(bridge_->inject_calls_, 0);
  drain();

  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "{\"messages\":[\"hello\"]}");
  EXPECT_EQ(posting_factory_.create_calls_, 0);
  // Nothing was ever awaiting a write, so the source was never paused for ingest back-pressure.
  EXPECT_EQ(bridge_->pause_source_calls_, 0);
}

// Crossing the in-memory limit creates the store and offloads everything held up to that point,
// so the replay comes back from storage.
TEST_F(BufferManagerTest, CrossingTheInMemoryLimitOffloadsEverything) {
  manager_config_.max_in_memory_bytes = 8;
  resetManager(posting_factory_);

  Buffer::OwnedImpl chunk1("12345678"); // Exactly at the limit, so still in memory.
  manager_->onData(chunk1);
  EXPECT_EQ(posting_factory_.create_calls_, 0);

  Buffer::OwnedImpl chunk2("9");
  manager_->onData(chunk2);
  ASSERT_EQ(posting_factory_.create_calls_, 1);

  manager_->endStream();
  drain();
  ASSERT_NE(posting_factory_.last_, nullptr);
  // One write carrying the whole payload, including the part that had been held in memory.
  ASSERT_EQ(posting_factory_.last_->write_calls_, 1);
  EXPECT_EQ(posting_factory_.last_->write_sizes_[0], 9);

  replayAll();
  drain();
  EXPECT_TRUE(replay_done_);
  EXPECT_EQ(bridge_->injected_.toString(), "123456789");
}

// Cancelling the stream coroutine handle cancels an in-flight external buffer read so late read
// completions are dropped.
TEST_F(BufferManagerTest, CancelTaskCancelsPendingExternalBufferStream) {
  resetManager(posting_factory_);

  Buffer::OwnedImpl body("message data for offload");
  manager_->onData(body);
  manager_->endStream();
  drain();

  replayAll();

  // streamPayload issued readChunk() immediately; PostingExternalBuffer posted the read callback,
  // so the read is currently in flight.
  EXPECT_FALSE(posted_.empty());

  // Now cancel the coroutine before the posted read callback completes.
  cancelActiveTask();

  // Run the remaining posted read callback.
  drain();

  EXPECT_FALSE(replay_done_);
  EXPECT_EQ(bridge_->injected_.length(), 0);
}

// Cancelling the inject coroutine handle disarms scheduled callbacks so nothing is injected.
TEST_F(BufferManagerTest, CancelTaskCancelsAPendingInject) {
  Buffer::OwnedImpl inject_data("{\"modified\":true}");
  bool inject_done = false;
  launchInject(*writer_, inject_data, [&inject_done](absl::Status) { inject_done = true; });

  EXPECT_FALSE(posted_.empty());
  cancelActiveTask();

  drain();
  EXPECT_FALSE(inject_done);
  EXPECT_EQ(bridge_->injected_.length(), 0);
}

// If the coroutine context is cancelled synchronously during injectData, draining stops
// immediately without touching subsequent chunks.
TEST_F(BufferManagerTest, SynchronousCancelStopsAnInjectImmediately) {
  Buffer::OwnedImpl body("initial");
  manager_->onData(body);
  manager_->endStream();
  drain();

  const std::string big_payload(200 * 1024, 'z'); // Multiple 64KB chunks
  Buffer::OwnedImpl inject_data(big_payload);
  bool inject_done = false;

  bridge_->on_inject_ = [this]() { cancelActiveTask(); };

  launchInject(*writer_, inject_data, [&inject_done](absl::Status) { inject_done = true; });

  drain();

  // Draining stopped after the first chunk was injected; no further chunks or done callback ran.
  EXPECT_EQ(bridge_->inject_calls_, 1);
  EXPECT_EQ(bridge_->injected_.length(), 64 * 1024);
  EXPECT_FALSE(inject_done);
}

// If streamPayload is cancelled synchronously during injectData, emission stops immediately.
TEST_F(BufferManagerTest, SynchronousCancelStopsExternalBufferStreamImmediately) {
  const std::string big_payload(200 * 1024, 'z');
  Buffer::OwnedImpl body(big_payload);
  manager_->onData(body);
  manager_->endStream();
  drain();

  bool replay_done = false;
  bridge_->on_inject_ = [this]() { cancelActiveTask(); };

  launchStream(*writer_, exportedReader(), 0, big_payload.size(),
               [&replay_done](absl::Status) { replay_done = true; });

  drain();

  // Stream stopped after the first chunk was injected.
  EXPECT_EQ(bridge_->inject_calls_, 1);
  EXPECT_EQ(bridge_->injected_.length(), 64 * 1024);
  EXPECT_FALSE(replay_done);
}

// Destroying a spilled BufferManager while writes are in-flight or pending releases its unacked
// byte count back to the bridge so ingest backpressure does not permanently deadlock the stream.
TEST_F(BufferManagerTest, DestroyReleasesUnackedBytesAndUnpausesSource) {
  bridge_buffer_limit_ = 1024;
  PostingExternalBufferFactory posting_factory;
  resetManager(posting_factory);

  // Feed 2048 bytes (> 1024 high watermark). Write is posted but not yet completed.
  Buffer::OwnedImpl body(std::string(2048, 'a'));
  manager_->onData(body);
  EXPECT_TRUE(bridge_->ingestPaused());
  EXPECT_EQ(bridge_->pause_source_calls_, 1);
  EXPECT_EQ(bridge_->resume_source_calls_, 0);

  // Destroy the manager before the write completes (e.g. an SSE frame store discarded).
  manager_->onDestroy();

  // Unacked bytes must be released and the source resumed immediately.
  EXPECT_FALSE(bridge_->ingestPaused());
  EXPECT_EQ(bridge_->resume_source_calls_, 1);
}

// If the FilterChainBridge is destroyed while a BufferManager and ChainWriter are still
// alive, the bridge detaches all registered managers so subsequent destruction does not access the
// destroyed bridge.
TEST_F(BufferManagerTest, BridgeDestructionDetachesRegisteredManagers) {
  auto local_bridge = std::make_unique<FakeBridge>(dispatcher_, 1024);
  auto local_manager =
      std::make_unique<BufferManager>(BufferManager::Config{}, factory_, *local_bridge);
  auto local_writer = std::make_unique<ChainWriter>(*local_bridge);

  Buffer::OwnedImpl body("payload");
  local_manager->onData(body);
  local_manager->endStream();

  // Destroy the bridge first (simulating filter teardown while coroutine still holds SseEvent).
  local_bridge.reset();

  // Destroying local_writer and local_manager afterwards must be a safe no-op.
  local_writer.reset();
  local_manager.reset();
}

// Cancelling the coroutine context and destroying ChainWriter, BufferManager, and caller's
// PayloadReader handle synchronously inside injectData() must not cause stack UAF when the stream
// coroutine unwinds.
TEST_F(BufferManagerTest, SynchronousDeallocationDuringInjectDataDoesNotUAF) {
  manager_config_.max_in_memory_bytes = 1024 * 1024;
  BufferManagerPtr local_manager = makeManager(factory_);
  auto local_writer = std::make_unique<ChainWriter>(*bridge_);

  Buffer::OwnedImpl body("hello world");
  local_manager->onData(body);
  local_manager->endStream();
  PayloadReaderSharedPtr local_reader =
      BufferManager::exportPayloadReader(std::move(local_manager));

  bridge_->on_inject_ = [this, &local_writer, &local_reader]() {
    cancelActiveTask();
    local_writer.reset();
    local_reader.reset();
  };

  launchStream(*local_writer, *local_reader, 0, 11, [](absl::Status) {});
  drain();

  EXPECT_EQ(local_writer, nullptr);
  EXPECT_EQ(local_manager, nullptr);
  EXPECT_EQ(local_reader, nullptr);
  EXPECT_EQ(bridge_->injected_.toString(), "hello world");
}

// Because PayloadReader is reference-counted and decoupled from BufferManager, dropping the
// caller's PayloadReaderSharedPtr synchronously inside injectData() while ChainWriter stays alive
// keeps the underlying PayloadReader alive until the active stream completes cleanly.
TEST_F(BufferManagerTest,
       SynchronousManagerAndCallerHandleDropDuringInjectDataCompletesReplaySafely) {
  manager_config_.max_in_memory_bytes = 1024 * 1024;
  BufferManagerPtr local_manager = makeManager(factory_);
  auto local_writer = std::make_unique<ChainWriter>(*bridge_);

  const std::string big(200 * 1024, 'a');
  Buffer::OwnedImpl body(big);
  local_manager->onData(body);
  local_manager->endStream();
  PayloadReaderSharedPtr local_reader =
      BufferManager::exportPayloadReader(std::move(local_manager));

  bool done_called = false;
  bridge_->on_inject_ = [&local_reader]() { local_reader.reset(); };

  launchStream(*local_writer, *local_reader, 0, big.size(),
               [&done_called](absl::Status s) { done_called = s.ok(); });
  drain();

  EXPECT_EQ(local_manager, nullptr);
  EXPECT_EQ(local_reader, nullptr);
  EXPECT_TRUE(done_called);
  EXPECT_EQ(bridge_->injected_.toString(), big);
}

// If a pending PayloadReader fails handoff while streamPayload is waiting for readability,
// streamPayload aborts with DataLossError.
TEST_F(BufferManagerTest, PayloadReaderWhenReadableErrorFailsStream) {
  PayloadReaderSharedPtr failing_reader = PayloadReader::createPendingExternal(10);
  bool done_called = false;
  launchStream(*writer_, *failing_reader, 0, 10,
               [&done_called](absl::Status) { done_called = true; });

  failing_reader->failExternalHandoff();
  drain();
  EXPECT_EQ(last_status_.code(), absl::StatusCode::kDataLoss);
  EXPECT_FALSE(done_called);
}

// When injection has yielded for the per-iteration inject budget, a watermark cycle while parked on
// the budget yield must not schedule a current-iteration callback.
TEST_F(BufferManagerTest, WatermarkCycleWhileBudgetYieldedDoesNotScheduleCurrentIteration) {
  manager_config_.max_in_memory_bytes = 1024 * 1024;
  resetManager(factory_);

  // 10 chunks of 64KB > 8 chunks per iteration budget.
  const std::string big_payload(10 * 64 * 1024, 'x');
  Buffer::OwnedImpl body(big_payload);
  manager_->onData(body);
  manager_->endStream();

  launchStream(*writer_, exportedReader(), 0, big_payload.size(), [](absl::Status) {});

  // First continuation consumes the 8-chunk budget and parks on a next-iteration callback.
  drain();
  EXPECT_EQ(bridge_->inject_calls_, 8);
  NiceMock<Event::MockSchedulableCallback>* budget_cb = replay_cb_;
  ASSERT_NE(budget_cb, nullptr);
  EXPECT_TRUE(budget_cb->enabled());

  // Simulate a watermark cycle while parked on the budget yield; no watermark waiter is registered,
  // so no current-iteration callback is posted or created.
  const size_t posted_before = posted_.size();
  bridge_->raiseWriteWatermark();
  bridge_->lowerWriteWatermark();
  EXPECT_EQ(replay_cb_, budget_cb);
  EXPECT_EQ(posted_.size(), posted_before);
}

// Write failure completion that triggers onUnrecoverableError() and drops the last external
// BufferManagerPtr while onWriteComplete() is on the call stack must not cause UAF.
TEST_F(BufferManagerTest, SynchronousDeallocationDuringWriteFailureDoesNotUAF) {
  manager_config_.max_in_memory_bytes = 0;
  FailingExternalBufferFactory failing_factory(FailingExternalBuffer::FailMode::Write);
  BufferManagerPtr local_manager = makeManager(failing_factory);

  bridge_->on_error_ = [&]() {
    bridge_->detachFromFilterChain();
    local_manager.reset();
  };

  Buffer::OwnedImpl body(std::string(128 * 1024, 'x'));
  local_manager->onData(body);
  drain();

  EXPECT_EQ(local_manager, nullptr);
  EXPECT_EQ(bridge_->error_calls_, 1);
}

// If onDestroy() releases unacked bytes, which lowers ingest backpressure and calls resumeSource(),
// and resumeSource() synchronously triggers bridge_.detachFromFilterChain(), unregistering from
// registered_managers_ before releaseUnacked() prevents an infinite loop in
// detachFromFilterChain().
TEST_F(BufferManagerTest, SynchronousDetachDuringResumeSourceInOnDestroyDoesNotInfiniteLoop) {
  bridge_buffer_limit_ = 1024;
  PostingExternalBufferFactory posting_factory;
  resetManager(posting_factory);

  Buffer::OwnedImpl body(std::string(2048, 'a'));
  manager_->onData(body);
  ASSERT_TRUE(bridge_->ingestPaused());

  bridge_->on_resume_source_ = [this]() { bridge_->detachFromFilterChain(); };

  manager_->onDestroy();
  EXPECT_EQ(bridge_->resume_source_calls_, 1);
}

// When a BufferManager holds a sub-WriteFlushThreshold pending backlog (> low_watermark_) while
// ingestPaused() is false, and a second charger (such as ScopedUnacked or another BufferManager)
// subsequently pushes unacked_ over high_watermark_, the bridge notifies registered BufferManagers
// to flush their pending backlog so unacked_ can fall back below low_watermark_.
TEST_F(BufferManagerTest, SubThresholdPendingFlushedWhenExternalChargerPausesIngest) {
  // high_watermark = 40 KiB, low_watermark = 20 KiB, WriteFlushThreshold = 64 KiB.
  bridge_buffer_limit_ = 40 * 1024;
  PostingExternalBufferFactory posting_factory;
  resetManager(posting_factory);

  // 30 KiB (> low_watermark, < high_watermark, < WriteFlushThreshold): queued in pending_ without
  // issuing a write because ingestPaused() is still false.
  Buffer::OwnedImpl chunk(std::string(30 * 1024, 'a'));
  manager_->onData(chunk);
  EXPECT_FALSE(bridge_->ingestPaused());
  EXPECT_EQ(posting_factory.last_->write_calls_, 0);

  // An external charger (e.g. pending_items_ ScopedUnacked) pushes unacked_ to 50 KiB > 40 KiB.
  // Transitioning to source_paused_ = true flushes manager_'s 30 KiB pending_ backlog.
  {
    FilterChainBridge::ScopedUnacked unacked(*bridge_, 20 * 1024);
    EXPECT_TRUE(bridge_->ingestPaused());
    EXPECT_EQ(posting_factory.last_->write_calls_, 1);
    drain();
  }

  // Once both the external ScopedUnacked and the flushed write complete, unacked_ drops to 0 <= 20
  // KiB and the source resumes instead of deadlocking at 30 KiB > 20 KiB.
  EXPECT_FALSE(bridge_->ingestPaused());
  EXPECT_EQ(bridge_->resume_source_calls_, 1);
}

// When exportPayloadReader() leaves a still-flushing BufferManager self-owned, dropping the
// PayloadReader causes the next write completion to skip any remaining backlog, release unacked
// bytes, and destroy the manager without surfacing errors.
TEST_F(BufferManagerTest, DroppingPendingPayloadReaderCleansUpSelfOwnedManagerOnWriteCompletion) {
  bridge_buffer_limit_ = 1024;
  PostingExternalBufferFactory posting_factory;
  resetManager(posting_factory);

  Buffer::OwnedImpl chunk1(std::string(2048, 'a'));
  manager_->onData(chunk1);
  Buffer::OwnedImpl chunk2(std::string(2048, 'b'));
  manager_->onData(chunk2, /*end_stream=*/true);
  ASSERT_TRUE(bridge_->ingestPaused());
  ASSERT_EQ(posting_factory.last_->write_calls_, 1);

  PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(manager_));
  ASSERT_EQ(manager_, nullptr);
  ASSERT_FALSE(reader->allBytesReadable());
  ASSERT_TRUE(bridge_->ingestPaused());

  reader.reset();
  drain();

  // The second queued chunk was not written after the reader expired, all unacked bytes were
  // released, and no error reached the bridge.
  EXPECT_EQ(posting_factory.last_->write_calls_, 1);
  EXPECT_FALSE(bridge_->ingestPaused());
  EXPECT_EQ(bridge_->resume_source_calls_, 1);
  EXPECT_EQ(bridge_->error_calls_, 0);
}

// Detaching or destroying the bridge while a self-owned BufferManager is still flushing must
// destroy the manager and fail the pending PayloadReader immediately, even if another holder
// retains the PayloadReaderSharedPtr past the bridge's lifetime.
TEST_F(BufferManagerTest, DetachingBridgeDestroysSelfOwnedManagerEvenWhileReaderSurvives) {
  PostingExternalBufferFactory posting_factory;
  auto local_bridge = std::make_unique<FakeBridge>(dispatcher_, /*buffer_limit=*/1024);
  auto local_manager =
      std::make_unique<BufferManager>(manager_config_, posting_factory, *local_bridge);

  Buffer::OwnedImpl body(std::string(2048, 'a'));
  local_manager->onData(body, /*end_stream=*/true);
  ASSERT_TRUE(local_bridge->ingestPaused());

  PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
  ASSERT_EQ(local_manager, nullptr);
  ASSERT_FALSE(reader->allBytesReadable());

  local_bridge->detachFromFilterChain();
  EXPECT_TRUE(reader->hasFailed());
  EXPECT_FALSE(reader->allBytesReadable());

  // Destroying the bridge before draining or dropping `reader` must not UAF.
  local_bridge.reset();
  drain();
  EXPECT_TRUE(reader->hasFailed());
}

// When a self-owned flushing BufferManager's write fails and triggers synchronous bridge
// detachment and reader drop inside onUnrecoverableError(), onWriteComplete() must not UAF.
TEST_F(BufferManagerTest, SelfOwnedWriteFailureWithSynchronousDetachDoesNotUAF) {
  manager_config_.max_in_memory_bytes = 0;
  FailingExternalBufferFactory failing_factory(FailingExternalBuffer::FailMode::Write);
  BufferManagerPtr local_manager = makeManager(failing_factory);

  Buffer::OwnedImpl body(std::string(128 * 1024, 'x'));
  local_manager->onData(body, /*end_stream=*/true);
  PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
  ASSERT_EQ(local_manager, nullptr);

  bridge_->on_error_ = [&]() {
    bridge_->detachFromFilterChain();
    reader.reset();
  };

  drain();
  EXPECT_EQ(reader, nullptr);
  EXPECT_EQ(bridge_->error_calls_, 1);
}

// A synchronous ExternalBuffer::write() failure triggered reentrantly from bridge_.addUnacked() ->
// onIngestPaused() that destroys the BufferManager must not UAF when addUnacked() unwinds.
TEST_F(BufferManagerTest, SynchronousWriteFailureDuringAddUnackedDoesNotUAF) {
  class SyncFailingWriteBuffer : public ExternalBuffer {
  public:
    void write(Buffer::InstancePtr, bool, WriteCallback cb) override {
      cb(ExternalBufferStatus::Error);
    }
    void read(uint64_t, uint64_t, ReadCallback cb) override {
      cb(ExternalBufferStatus::Error, nullptr);
    }
    uint64_t length() const override { return 0; }
  };
  class SyncFailingWriteFactory : public ExternalBufferFactory {
  public:
    ExternalBufferPtr createBuffer(Event::Dispatcher&) override {
      return std::make_unique<SyncFailingWriteBuffer>();
    }
  };

  bridge_buffer_limit_ = 1024;
  manager_config_.max_in_memory_bytes = 0;
  SyncFailingWriteFactory sync_failing_factory;
  BufferManagerPtr local_manager = makeManager(sync_failing_factory);

  bridge_->on_error_ = [&]() {
    bridge_->detachFromFilterChain();
    local_manager.reset();
  };

  Buffer::OwnedImpl body(std::string(2048, 'x'));
  local_manager->onData(body);

  EXPECT_EQ(local_manager, nullptr);
  EXPECT_EQ(bridge_->error_calls_, 1);
}

// Calling onData() or endStream() after onDestroy() must drain the input and never resurrect an
// ExternalBuffer on a destroyed BufferManager.
TEST_F(BufferManagerTest, OnDataAndEndStreamAfterDestroyAreNoOps) {
  class CountingFactory : public ExternalBufferFactory {
  public:
    ExternalBufferPtr createBuffer(Event::Dispatcher& dispatcher) override {
      ++create_calls;
      return inner.createBuffer(dispatcher);
    }
    InMemoryExternalBufferFactory inner;
    int create_calls{0};
  };

  CountingFactory counting_factory;
  manager_config_.max_in_memory_bytes = 0;
  resetManager(counting_factory);
  manager_->onDestroy();

  Buffer::OwnedImpl body("late bytes");
  manager_->onData(body);
  manager_->endStream();

  EXPECT_EQ(body.length(), 0);
  EXPECT_EQ(counting_factory.create_calls, 0);
  EXPECT_FALSE(bridge_->ingestPaused());
}

// Calling endStream() or onData(..., /*end_stream=*/true) followed by exportPayloadReader() after
// a write failure or onDestroy() must return a failed PayloadReader without hitting
// ASSERT(end_stream_seen_).
TEST_F(BufferManagerTest, ExportAfterWriteFailureAndAfterDestroyReturnsFailedReader) {
  manager_config_.max_in_memory_bytes = 0;

  // 1) After an async write failure + endStream().
  {
    FailingExternalBufferFactory failing_factory(FailingExternalBuffer::FailMode::Write);
    BufferManagerPtr local_manager = makeManager(failing_factory);
    Buffer::OwnedImpl body(std::string(64 * 1024, 'x'));
    local_manager->onData(body);
    drain();
    local_manager->endStream();
    PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasFailed());
    EXPECT_EQ(reader->length(), 64 * 1024);
  }

  // 2) After an async write failure + onData(..., /*end_stream=*/true).
  {
    FailingExternalBufferFactory failing_factory(FailingExternalBuffer::FailMode::Write);
    BufferManagerPtr local_manager = makeManager(failing_factory);
    Buffer::OwnedImpl chunk1(std::string(64 * 1024, 'x'));
    local_manager->onData(chunk1);
    drain();
    Buffer::OwnedImpl chunk2("tail");
    local_manager->onData(chunk2, /*end_stream=*/true);
    EXPECT_EQ(chunk2.length(), 0);
    PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasFailed());
  }

  // 3) After onDestroy() + endStream().
  {
    BufferManagerPtr local_manager = makeManager(factory_);
    local_manager->onDestroy();
    local_manager->endStream();
    PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->hasFailed());
  }
}

// A synchronous ExternalBuffer::write(Ok) on a follow-up write of a self-owned BufferManager must
// complete the PayloadReader handoff and tear down self_owner_ cleanly without UAF.
TEST_F(BufferManagerTest, SynchronousWriteOkCompletesSelfOwnedFollowUpWrite) {
  class HybridSyncOkBuffer : public ExternalBuffer {
  public:
    void write(Buffer::InstancePtr data, bool, WriteCallback cb) override {
      stored_ += data->toString();
      if (write_count_++ == 0) {
        pending_cb_ = std::move(cb);
        return;
      }
      cb(ExternalBufferStatus::Ok);
    }
    void read(uint64_t offset, uint64_t length, ReadCallback cb) override {
      auto out = std::make_unique<Buffer::OwnedImpl>();
      out->add(stored_.data() + offset, length);
      cb(ExternalBufferStatus::Ok, std::move(out));
    }
    uint64_t length() const override { return stored_.size(); }

    void completeFirstWrite() {
      WriteCallback cb = std::move(pending_cb_);
      pending_cb_ = nullptr;
      ASSERT_NE(cb, nullptr);
      cb(ExternalBufferStatus::Ok);
    }

  private:
    std::string stored_;
    int write_count_{0};
    WriteCallback pending_cb_;
  };
  class HybridSyncOkFactory : public ExternalBufferFactory {
  public:
    ExternalBufferPtr createBuffer(Event::Dispatcher&) override {
      auto buf = std::make_unique<HybridSyncOkBuffer>();
      last_buffer = buf.get();
      return buf;
    }
    HybridSyncOkBuffer* last_buffer{nullptr};
  };

  HybridSyncOkFactory hybrid_factory;
  manager_config_.max_in_memory_bytes = 0;
  BufferManagerPtr local_manager = makeManager(hybrid_factory);

  const std::string part1(64 * 1024, 'a');
  const std::string part2 = "tail-bytes";
  Buffer::OwnedImpl b1(part1);
  local_manager->onData(b1, /*end_stream=*/false);
  ASSERT_NE(hybrid_factory.last_buffer, nullptr);

  Buffer::OwnedImpl b2(part2);
  local_manager->onData(b2, /*end_stream=*/true);

  PayloadReaderSharedPtr reader = BufferManager::exportPayloadReader(std::move(local_manager));
  ASSERT_EQ(local_manager, nullptr);
  EXPECT_FALSE(reader->allBytesReadable());

  // Completing write #1 triggers write #2 synchronously inside maybeIssueWrite(), which completes
  // handoff and destroys self_owner_ before the outer onWriteComplete() returns.
  hybrid_factory.last_buffer->completeFirstWrite();
  EXPECT_TRUE(reader->allBytesReadable());
  EXPECT_FALSE(reader->hasFailed());
  EXPECT_EQ(reader->length(), part1.size() + part2.size());

  absl::StatusOr<Buffer::InstancePtr> read_or = absl::UnknownError("pending");
  auto handle = Coroutine::launch(
      reader->readChunk(0, reader->length()), executor_,
      [&read_or](absl::StatusOr<Buffer::InstancePtr> res) { read_or = std::move(res); },
      Coroutine::StartMode::Inline);
  ASSERT_THAT(read_or.status(), StatusHelpers::IsOk());
  EXPECT_EQ((*read_or)->toString(), part1 + part2);
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
