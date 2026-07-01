#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/event/dispatcher.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/logger.h"
#include "source/extensions/common/async_files/async_file_handle.h"
#include "source/extensions/common/async_files/async_file_manager.h"
#include "source/extensions/common/async_files/async_file_manager_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"

#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// An ExternalBuffer backed by a single anonymous file on the local filesystem.
// The payload is offloaded to disk (via the thread-pool AsyncFileManager, off the
// worker event loop) so the resident memory footprint stays bounded to the
// in-flight window. The file is created unlinked (O_TMPFILE-style), so it never
// appears in a directory listing and its blocks are reclaimed automatically when
// the handle is closed or the process exits -- no cleanup path is needed.
//
// The BufferManager drives this store with at most one write() and at most one
// read() outstanding, and never overlaps a read with a write (reads only begin
// after the offload is fully durable). That single-op-at-a-time contract matches
// the AsyncFileContext requirement of at most one action queued per handle.
//
// Because the file is opened asynchronously, a write() may arrive before the open
// completes; that first write is stashed and issued once the handle is ready.
class FilesystemExternalBuffer : public ExternalBuffer,
                                 public Logger::Loggable<Logger::Id::filter> {
public:
  FilesystemExternalBuffer(Common::AsyncFiles::AsyncFileManager& manager, std::string path,
                           Event::Dispatcher& dispatcher);
  ~FilesystemExternalBuffer() override;

  // ExternalBuffer
  void write(Buffer::InstancePtr data, WriteCallback cb) override;
  void read(uint64_t offset, uint64_t length, ReadCallback cb) override;
  uint64_t length() const override { return length_; }

private:
  enum class State {
    // The anonymous file open is in flight; no handle yet.
    Opening,
    // The handle is open and ready for I/O.
    Ready,
    // An open or I/O error occurred; the buffer is unusable and every further
    // operation reports Error.
    Failed,
  };

  // Completion of the createAnonymousFile() started in the constructor. Transitions
  // to Ready (draining any stashed write) or Failed.
  void onOpenComplete(absl::StatusOr<Common::AsyncFiles::AsyncFileHandle> handle);
  // Issues a write of `data` at the current durable end (length_), advancing
  // length_ only once the write is acknowledged.
  void issueWrite(Buffer::InstancePtr data, WriteCallback cb);

  Common::AsyncFiles::AsyncFileManager& manager_;
  Event::Dispatcher& dispatcher_;
  const std::string path_;
  Common::AsyncFiles::AsyncFileHandle handle_;
  State state_{State::Opening};

  // Durable, acknowledged byte count. Also the offset at which the next write
  // appends. Advanced only inside a write completion (before its callback fires),
  // so length() never reports bytes whose write has not yet been acknowledged.
  uint64_t length_{0};

  // A write() that arrived while still Opening, held until the handle is ready.
  // The BufferManager keeps at most one write outstanding, so a single slot
  // suffices.
  struct PendingWrite {
    Buffer::InstancePtr data;
    WriteCallback cb;
  };
  std::optional<PendingWrite> pending_write_;

  // Cancels the in-flight open (closing the file if it was already opened) / the
  // in-flight read or write. Used on destruction to abort work promptly; the
  // alive_ guard below independently neutralizes any completion already posted.
  Common::AsyncFiles::CancelFunction cancel_open_;
  Common::AsyncFiles::CancelFunction cancel_io_;

  // Set to false in the destructor and captured (by value) into every completion
  // callback, so a callback posted before destruction becomes a no-op instead of
  // touching freed state.
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

// An ExternalBuffer that keeps small payloads entirely in memory and only spills
// to a file-backed buffer once the offloaded size exceeds a threshold. This
// avoids the file open and I/O for the common case of small requests while still
// bounding the resident footprint of large ones.
//
// The BufferManager performs every write (offload) before any read (replay), so
// by read time the buffer is in a single stable tier: either still in memory (the
// payload never exceeded the threshold) or fully migrated to a file (it did).
// When the threshold is crossed, the buffered head and the frame that crossed it
// are handed to a FilesystemExternalBuffer in a single write, and all subsequent
// writes and reads are forwarded to it.
class TieredExternalBuffer : public ExternalBuffer, public Logger::Loggable<Logger::Id::filter> {
public:
  TieredExternalBuffer(Common::AsyncFiles::AsyncFileManager& manager, std::string path,
                       uint64_t memory_threshold, Event::Dispatcher& dispatcher);
  ~TieredExternalBuffer() override;

  // ExternalBuffer
  void write(Buffer::InstancePtr data, WriteCallback cb) override;
  void read(uint64_t offset, uint64_t length, ReadCallback cb) override;
  uint64_t length() const override { return length_; }

private:
  // Writes `data` to the file tier, advancing length_ by `new_bytes` (the not-yet-
  // counted bytes in `data`) once the write is acknowledged. Used both for the
  // spill write (head already counted, so new_bytes is just the crossing frame)
  // and for post-spill writes (the whole frame).
  void writeToFile(Buffer::InstancePtr data, uint64_t new_bytes, WriteCallback cb);

  Common::AsyncFiles::AsyncFileManager& manager_;
  const std::string path_;
  const uint64_t threshold_;
  Event::Dispatcher& dispatcher_;

  // Buffered bytes while still in the memory tier; drained into the file on spill.
  Buffer::OwnedImpl memory_;
  // The file-backed tier, created when the threshold is crossed. Null while in
  // memory.
  ExternalBufferPtr file_;
  bool spilled_{false};

  // Durable, acknowledged byte count across both tiers.
  uint64_t length_{0};

  // Neutralizes posted memory-write completions after destruction (the file tier
  // guards its own callbacks).
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

// Factory for the filesystem-backed store. Holds the shared AsyncFileManager (a
// thread-safe singleton) and the AsyncFileManagerFactory that owns it, the scratch
// directory, and the in-memory spill threshold. Stateless with respect to streams
// and safe to share across workers, exactly like InMemoryExternalBufferFactory.
//
// When the threshold is zero the payload is file-backed from the first byte (a
// plain FilesystemExternalBuffer); otherwise a TieredExternalBuffer keeps small
// payloads in memory and spills past the threshold.
class FilesystemExternalBufferFactory : public ExternalBufferFactory {
public:
  FilesystemExternalBufferFactory(
      std::shared_ptr<Common::AsyncFiles::AsyncFileManagerFactory> manager_factory,
      std::shared_ptr<Common::AsyncFiles::AsyncFileManager> manager, std::string path,
      uint64_t memory_threshold)
      : manager_factory_(std::move(manager_factory)), manager_(std::move(manager)),
        path_(std::move(path)), memory_threshold_(memory_threshold) {}

  ExternalBufferPtr createBuffer(Event::Dispatcher& dispatcher) override {
    if (memory_threshold_ == 0) {
      return std::make_unique<FilesystemExternalBuffer>(*manager_, path_, dispatcher);
    }
    return std::make_unique<TieredExternalBuffer>(*manager_, path_, memory_threshold_, dispatcher);
  }

private:
  // Retained so the id->manager registry is not torn down while streams are live
  // (the singleton manager does not itself keep the factory alive).
  std::shared_ptr<Common::AsyncFiles::AsyncFileManagerFactory> manager_factory_;
  std::shared_ptr<Common::AsyncFiles::AsyncFileManager> manager_;
  const std::string path_;
  const uint64_t memory_threshold_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
