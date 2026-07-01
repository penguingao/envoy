#include <memory>
#include <string>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/singleton/manager_impl.h"
#include "source/extensions/common/async_files/async_file_manager.h"
#include "source/extensions/common/async_files/async_file_manager_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/filesystem_external_buffer.h"

#include "test/extensions/common/async_files/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using Common::AsyncFiles::AsyncFileHandle;
using Common::AsyncFiles::AsyncFileManager;
using Common::AsyncFiles::AsyncFileManagerFactory;
using Common::AsyncFiles::MockAsyncFileContext;
using Common::AsyncFiles::MockAsyncFileManager;
using ::testing::_;

// ----------------------------------------------------------------------------
// Round-trip tests against a real thread-pool AsyncFileManager.
// ----------------------------------------------------------------------------
class FilesystemExternalBufferTest : public testing::Test {
public:
  void SetUp() override {
    singleton_manager_ = std::make_unique<Singleton::ManagerImpl>();
    factory_ = AsyncFileManagerFactory::singleton(singleton_manager_.get());
    envoy::extensions::common::async_files::v3::AsyncFileManagerConfig config;
    config.mutable_thread_pool()->set_thread_count(1);
    manager_ = factory_->getAsyncFileManager(config);
    tmpdir_ = TestEnvironment::temporaryDirectory();
  }

  std::unique_ptr<FilesystemExternalBuffer> makeBuffer() {
    return std::make_unique<FilesystemExternalBuffer>(*manager_, tmpdir_, *dispatcher_);
  }

  // Drives the thread pool and the event loop one step: waits for queued file
  // actions to finish and their completions to be posted, then runs them (which
  // may enqueue the next action in a chain).
  void drive() {
    manager_->waitForIdle();
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

  // Writes one chunk and drives until it is acknowledged (the store keeps at most
  // one write outstanding, so callers write one chunk at a time).
  void writeChunk(ExternalBuffer& buffer, absl::string_view chunk) {
    bool acked = false;
    buffer.write(std::make_unique<Buffer::OwnedImpl>(chunk), [&acked](ExternalBufferStatus status) {
      EXPECT_EQ(status, ExternalBufferStatus::Ok);
      acked = true;
    });
    for (int i = 0; i < 10 && !acked; i++) {
      drive();
    }
    EXPECT_TRUE(acked);
  }

  std::string readRange(ExternalBuffer& buffer, uint64_t offset, uint64_t length) {
    std::string out;
    bool done = false;
    buffer.read(offset, length,
                [&out, &done](ExternalBufferStatus status, Buffer::InstancePtr data) {
                  EXPECT_EQ(status, ExternalBufferStatus::Ok);
                  if (data != nullptr) {
                    out = data->toString();
                  }
                  done = true;
                });
    for (int i = 0; i < 10 && !done; i++) {
      drive();
    }
    EXPECT_TRUE(done);
    return out;
  }

protected:
  // Declared first so they outlive manager_: destroying the thread-pool manager
  // drains queued actions and posts their completions to the dispatcher, so the
  // dispatcher (and api) must still be alive at that point. Members are destroyed
  // in reverse declaration order, so manager_/factory_/singleton_manager_ (below)
  // are torn down before the dispatcher.
  Api::ApiPtr api_ = Api::createApiForTest();
  Event::DispatcherPtr dispatcher_ = api_->allocateDispatcher("test");
  std::unique_ptr<Singleton::ManagerImpl> singleton_manager_;
  std::shared_ptr<AsyncFileManagerFactory> factory_;
  std::shared_ptr<AsyncFileManager> manager_;
  std::string tmpdir_;
};

// Successive writes accumulate in order; a read of the whole range returns the
// bytes verbatim off disk.
TEST_F(FilesystemExternalBufferTest, WriteThenReadRoundTrips) {
  auto buffer = makeBuffer();
  writeChunk(*buffer, "abc");
  writeChunk(*buffer, "def");
  writeChunk(*buffer, "ghij");
  EXPECT_EQ(buffer->length(), 10);
  EXPECT_EQ(readRange(*buffer, 0, 10), "abcdefghij");
}

// The write is not durable until the file I/O completes: length() reflects only
// acknowledged bytes.
TEST_F(FilesystemExternalBufferTest, WriteIsNotDurableUntilAcknowledged) {
  auto buffer = makeBuffer();
  bool acked = false;
  buffer->write(std::make_unique<Buffer::OwnedImpl>("hello"),
                [&acked](ExternalBufferStatus status) {
                  EXPECT_EQ(status, ExternalBufferStatus::Ok);
                  acked = true;
                });
  // Nothing has been driven yet: the open has not even completed.
  EXPECT_FALSE(acked);
  EXPECT_EQ(buffer->length(), 0);

  for (int i = 0; i < 10 && !acked; i++) {
    drive();
  }
  EXPECT_TRUE(acked);
  EXPECT_EQ(buffer->length(), 5);
}

// Reads honor arbitrary byte offsets and are non-destructive (repeatable).
TEST_F(FilesystemExternalBufferTest, ReadAtOffsetIsRepeatable) {
  auto buffer = makeBuffer();
  writeChunk(*buffer, "0123456789");

  EXPECT_EQ(readRange(*buffer, 3, 4), "3456");
  EXPECT_EQ(readRange(*buffer, 3, 4), "3456");
  EXPECT_EQ(buffer->length(), 10); // unchanged by reads.
}

// A zero-length read completes with an empty buffer without touching the file.
TEST_F(FilesystemExternalBufferTest, EmptyReadReturnsNoBytes) {
  auto buffer = makeBuffer();
  writeChunk(*buffer, "data");

  bool done = false;
  buffer->read(0, 0, [&done](ExternalBufferStatus status, Buffer::InstancePtr data) {
    EXPECT_EQ(status, ExternalBufferStatus::Ok);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->length(), 0);
    done = true;
  });
  // Completes synchronously, no file I/O needed.
  EXPECT_TRUE(done);
}

// Destroying the buffer with a write still in flight must not invoke its
// callback, and must not leak or use freed state (ASAN).
TEST_F(FilesystemExternalBufferTest, PendingCallbacksCancelledOnDestruction) {
  bool acked = false;
  {
    auto buffer = makeBuffer();
    buffer->write(std::make_unique<Buffer::OwnedImpl>("data"),
                  [&acked](ExternalBufferStatus) { acked = true; });
    // buffer is destroyed here, before the open/write is driven.
  }
  for (int i = 0; i < 10; i++) {
    drive();
  }
  EXPECT_FALSE(acked);
}

// ----------------------------------------------------------------------------
// TieredExternalBuffer: in-memory below the threshold, file-backed above it.
// ----------------------------------------------------------------------------

// A payload that stays under the threshold round-trips entirely from memory.
TEST_F(FilesystemExternalBufferTest, TieredStaysInMemoryBelowThreshold) {
  TieredExternalBuffer buffer(*manager_, tmpdir_, /*memory_threshold=*/1000, *dispatcher_);
  writeChunk(buffer, "abc");
  writeChunk(buffer, "defg");
  EXPECT_EQ(buffer.length(), 7);
  EXPECT_EQ(readRange(buffer, 0, 7), "abcdefg");
  EXPECT_EQ(readRange(buffer, 2, 3), "cde");
}

// Crossing the threshold across successive writes spills to a file, and the whole
// payload (buffered head plus the spilled tail) reads back verbatim.
TEST_F(FilesystemExternalBufferTest, TieredSpillsWhenThresholdCrossed) {
  TieredExternalBuffer buffer(*manager_, tmpdir_, /*memory_threshold=*/4, *dispatcher_);
  writeChunk(buffer, "abc");   // 3 bytes: stays in memory.
  writeChunk(buffer, "defgh"); // total 8 > 4: spills head + this frame to a file.
  writeChunk(buffer, "ij");    // appended to the file tier.
  EXPECT_EQ(buffer.length(), 10);
  EXPECT_EQ(readRange(buffer, 0, 10), "abcdefghij");
  EXPECT_EQ(readRange(buffer, 3, 4), "defg");
}

// A single first write larger than the threshold spills immediately.
TEST_F(FilesystemExternalBufferTest, TieredSingleLargeWriteSpills) {
  TieredExternalBuffer buffer(*manager_, tmpdir_, /*memory_threshold=*/4, *dispatcher_);
  writeChunk(buffer, "0123456789");
  EXPECT_EQ(buffer.length(), 10);
  EXPECT_EQ(readRange(buffer, 0, 10), "0123456789");
}

// A write that exactly reaches the threshold stays in memory; the next byte spills.
TEST_F(FilesystemExternalBufferTest, TieredThresholdBoundaryIsInclusive) {
  TieredExternalBuffer buffer(*manager_, tmpdir_, /*memory_threshold=*/5, *dispatcher_);
  writeChunk(buffer, "abcde"); // exactly 5: still in memory.
  EXPECT_EQ(buffer.length(), 5);
  writeChunk(buffer, "f"); // 6 > 5: spills.
  EXPECT_EQ(buffer.length(), 6);
  EXPECT_EQ(readRange(buffer, 0, 6), "abcdef");
}

// ----------------------------------------------------------------------------
// Fault-injection tests against a mock AsyncFileManager.
// ----------------------------------------------------------------------------
class FilesystemExternalBufferMockTest : public testing::Test {
public:
  // A fresh mock context. Its constructor sets an EXPECT_CALL that close() is
  // invoked exactly once, so any test that opens successfully with this handle
  // also asserts the handle is closed on destruction.
  std::shared_ptr<testing::NiceMock<MockAsyncFileContext>> makeHandle() {
    return std::make_shared<testing::NiceMock<MockAsyncFileContext>>(manager_);
  }

  // Completes the pending open action with `handle` and runs the loop.
  void completeOpenOk(AsyncFileHandle handle) {
    manager_->nextActionCompletes(absl::StatusOr<AsyncFileHandle>(std::move(handle)));
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

protected:
  std::shared_ptr<testing::NiceMock<MockAsyncFileManager>> manager_ =
      std::make_shared<testing::NiceMock<MockAsyncFileManager>>();
  Api::ApiPtr api_ = Api::createApiForTest();
  Event::DispatcherPtr dispatcher_ = api_->allocateDispatcher("test");
};

// A tiered buffer that stays under the threshold must never open a file.
TEST_F(FilesystemExternalBufferMockTest, TieredBelowThresholdNeverOpensFile) {
  EXPECT_CALL(*manager_, createAnonymousFile(_, _, _)).Times(0);
  TieredExternalBuffer buffer(*manager_, "/tmp", /*memory_threshold=*/100, *dispatcher_);

  bool acked = false;
  buffer.write(std::make_unique<Buffer::OwnedImpl>("small"), [&acked](ExternalBufferStatus status) {
    EXPECT_EQ(status, ExternalBufferStatus::Ok);
    acked = true;
  });
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_TRUE(acked);
  ASSERT_EQ(buffer.length(), 5);

  std::string read_back;
  buffer.read(0, 5, [&read_back](ExternalBufferStatus status, Buffer::InstancePtr data) {
    EXPECT_EQ(status, ExternalBufferStatus::Ok);
    read_back = data->toString();
  });
  EXPECT_EQ(read_back, "small");
}

// A failed open surfaces as Error on the write that was stashed while opening.
// No handle is produced, so nothing is closed.
TEST_F(FilesystemExternalBufferMockTest, OpenFailureFailsStashedWrite) {
  FilesystemExternalBuffer buffer(*manager_, "/tmp", *dispatcher_);

  ExternalBufferStatus result = ExternalBufferStatus::Ok;
  bool acked = false;
  buffer.write(std::make_unique<Buffer::OwnedImpl>("data"), [&](ExternalBufferStatus status) {
    result = status;
    acked = true;
  });

  manager_->nextActionCompletes(
      absl::StatusOr<AsyncFileHandle>(absl::InternalError("open failed")));
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_TRUE(acked);
  EXPECT_EQ(result, ExternalBufferStatus::Error);
  EXPECT_EQ(buffer.length(), 0);
}

// A failed write surfaces as Error and leaves length() unchanged.
TEST_F(FilesystemExternalBufferMockTest, WriteFailureReportsError) {
  auto handle = makeHandle();
  FilesystemExternalBuffer buffer(*manager_, "/tmp", *dispatcher_);
  completeOpenOk(handle);

  ExternalBufferStatus result = ExternalBufferStatus::Ok;
  buffer.write(std::make_unique<Buffer::OwnedImpl>("data"),
               [&result](ExternalBufferStatus status) { result = status; });
  manager_->nextActionCompletes(absl::StatusOr<size_t>(absl::InternalError("write failed")));
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_EQ(result, ExternalBufferStatus::Error);
  EXPECT_EQ(buffer.length(), 0);
}

// A short read (fewer bytes than requested inside the durable range) is treated
// as an error.
TEST_F(FilesystemExternalBufferMockTest, ShortReadReportsError) {
  auto handle = makeHandle();
  FilesystemExternalBuffer buffer(*manager_, "/tmp", *dispatcher_);
  completeOpenOk(handle);

  // Make four bytes durable.
  buffer.write(std::make_unique<Buffer::OwnedImpl>("data"), [](ExternalBufferStatus) {});
  manager_->nextActionCompletes(absl::StatusOr<size_t>(size_t{4}));
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  ASSERT_EQ(buffer.length(), 4);

  ExternalBufferStatus result = ExternalBufferStatus::Ok;
  Buffer::InstancePtr returned = std::make_unique<Buffer::OwnedImpl>("sentinel");
  buffer.read(0, 4, [&](ExternalBufferStatus status, Buffer::InstancePtr data) {
    result = status;
    returned = std::move(data);
  });
  // Return only two of the four requested bytes.
  manager_->nextActionCompletes(
      absl::StatusOr<Buffer::InstancePtr>(std::make_unique<Buffer::OwnedImpl>("da")));
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_EQ(result, ExternalBufferStatus::Error);
  EXPECT_EQ(returned, nullptr);
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
