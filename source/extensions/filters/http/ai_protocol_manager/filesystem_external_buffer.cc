#include "source/extensions/filters/http/ai_protocol_manager/filesystem_external_buffer.h"

#include <memory>
#include <utility>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using Common::AsyncFiles::AsyncFileHandle;

FilesystemExternalBuffer::FilesystemExternalBuffer(Common::AsyncFiles::AsyncFileManager& manager,
                                                   std::string path, Event::Dispatcher& dispatcher)
    : manager_(manager), dispatcher_(dispatcher), path_(std::move(path)) {
  // Open eagerly so the open latency overlaps header processing rather than
  // serializing in front of the first write. A write() arriving before this
  // completes is stashed in pending_write_ and drained by onOpenComplete().
  cancel_open_ = manager_.createAnonymousFile(
      &dispatcher_, path_, [this, alive = alive_](absl::StatusOr<AsyncFileHandle> handle) {
        if (!*alive) {
          return;
        }
        onOpenComplete(std::move(handle));
      });
}

FilesystemExternalBuffer::~FilesystemExternalBuffer() {
  // Neutralize any completion callback that was posted but has not run yet.
  *alive_ = false;
  // Abort in-flight work. Cancelling the open also closes the file if the pool
  // thread had already opened it (we hold no handle to close in that case).
  if (cancel_open_) {
    cancel_open_();
  }
  if (cancel_io_) {
    cancel_io_();
  }
  // The handle must be closed before it is dropped. close() is safe to enqueue
  // from here: the handle is a shared_ptr and the close action keeps it alive
  // until the fd is actually closed, so we can release our reference. The
  // completion captures nothing, so it stays valid even though alive_ is now
  // false. The anonymous file is unlinked, so closing it also reclaims the disk.
  if (handle_ != nullptr) {
    auto status = handle_->close(&dispatcher_, [](absl::Status) {});
    // close() only fails its precondition if the file was already closed, which
    // never happens here (we close exactly once, at destruction).
    ASSERT(status.ok());
  }
}

void FilesystemExternalBuffer::onOpenComplete(absl::StatusOr<AsyncFileHandle> handle) {
  cancel_open_ = nullptr;
  if (!handle.ok()) {
    ENVOY_LOG(warn, "ai_protocol_manager: failed to open buffer file in '{}': {}", path_,
              handle.status().message());
    state_ = State::Failed;
    if (pending_write_.has_value()) {
      WriteCallback cb = std::move(pending_write_->cb);
      pending_write_.reset();
      cb(ExternalBufferStatus::Error);
    }
    return;
  }
  handle_ = std::move(*handle);
  state_ = State::Ready;
  ENVOY_LOG(trace, "ai_protocol_manager: buffer file opened in '{}'", path_);
  // Drain a write that arrived while opening.
  if (pending_write_.has_value()) {
    PendingWrite pending = std::move(*pending_write_);
    pending_write_.reset();
    issueWrite(std::move(pending.data), std::move(pending.cb));
  }
}

void FilesystemExternalBuffer::write(Buffer::InstancePtr data, WriteCallback cb) {
  switch (state_) {
  case State::Opening:
    // At most one write is outstanding (BufferManager contract), so the single
    // stash slot is always free here.
    ASSERT(!pending_write_.has_value());
    pending_write_ = PendingWrite{std::move(data), std::move(cb)};
    return;
  case State::Ready:
    issueWrite(std::move(data), std::move(cb));
    return;
  case State::Failed:
    cb(ExternalBufferStatus::Error);
    return;
  }
}

void FilesystemExternalBuffer::issueWrite(Buffer::InstancePtr data, WriteCallback cb) {
  const uint64_t submitted = data->length();
  auto result = handle_->write(
      &dispatcher_, *data, static_cast<off_t>(length_),
      [this, alive = alive_, submitted, cb = std::move(cb)](absl::StatusOr<size_t> n) mutable {
        if (!*alive) {
          return;
        }
        cancel_io_ = nullptr;
        // The thread-pool write loops internally until the whole buffer is
        // flushed, so success implies a full write; a short count still means
        // trouble and is treated as an error.
        if (!n.ok() || *n != submitted) {
          ENVOY_LOG(warn, "ai_protocol_manager: buffer file write failed");
          state_ = State::Failed;
          cb(ExternalBufferStatus::Error);
          return;
        }
        // Now durable: expose the bytes to length()/read() and acknowledge.
        length_ += *n;
        cb(ExternalBufferStatus::Ok);
      });
  // write() consumes *data by move; the owning unique_ptr is dropped on return.
  // The outer status only fails if the file is already closed, which cannot
  // happen at Ready -- but the callback has already been moved into the action,
  // so we cannot re-invoke it; assert and bail rather than dereference a bad
  // StatusOr.
  ASSERT(result.ok());
  if (!result.ok()) {
    state_ = State::Failed;
    return;
  }
  cancel_io_ = std::move(*result);
}

void FilesystemExternalBuffer::read(uint64_t offset, uint64_t length, ReadCallback cb) {
  // Reads only happen during replay, after every write is durable; the range must
  // lie within the acknowledged length.
  ASSERT(state_ == State::Ready);
  ASSERT(offset + length <= length_);
  if (length == 0) {
    cb(ExternalBufferStatus::Ok, std::make_unique<Buffer::OwnedImpl>());
    return;
  }
  auto result = handle_->read(&dispatcher_, static_cast<off_t>(offset), static_cast<size_t>(length),
                              [this, alive = alive_, length, cb = std::move(cb)](
                                  absl::StatusOr<Buffer::InstancePtr> data) mutable {
                                if (!*alive) {
                                  return;
                                }
                                cancel_io_ = nullptr;
                                // The requested range is inside a fully-written file, so a short
                                // read signals truncation/corruption; the ReadCallback contract
                                // requires exactly the requested bytes, so treat anything else as
                                // an error.
                                if (!data.ok() || *data == nullptr || (*data)->length() != length) {
                                  ENVOY_LOG(warn, "ai_protocol_manager: buffer file read failed");
                                  state_ = State::Failed;
                                  cb(ExternalBufferStatus::Error, nullptr);
                                  return;
                                }
                                cb(ExternalBufferStatus::Ok, std::move(*data));
                              });
  ASSERT(result.ok());
  if (!result.ok()) {
    state_ = State::Failed;
    return;
  }
  cancel_io_ = std::move(*result);
}

TieredExternalBuffer::TieredExternalBuffer(Common::AsyncFiles::AsyncFileManager& manager,
                                           std::string path, uint64_t memory_threshold,
                                           Event::Dispatcher& dispatcher)
    : manager_(manager), path_(std::move(path)), threshold_(memory_threshold),
      dispatcher_(dispatcher) {}

TieredExternalBuffer::~TieredExternalBuffer() {
  // Neutralize any posted memory-write completion. The file tier (if any) guards
  // its own callbacks and is torn down with this object.
  *alive_ = false;
}

void TieredExternalBuffer::write(Buffer::InstancePtr data, WriteCallback cb) {
  const uint64_t incoming = data->length();

  if (spilled_) {
    writeToFile(std::move(data), incoming, std::move(cb));
    return;
  }

  if (length_ + incoming <= threshold_) {
    // Stay in the memory tier. Take the bytes now, but post the acknowledgment
    // (like InMemoryExternalBuffer) so it is not delivered reentrantly and
    // length() reflects only acknowledged bytes.
    memory_.move(*data);
    dispatcher_.post([this, alive = alive_, incoming, cb = std::move(cb)]() mutable {
      if (!*alive) {
        return;
      }
      length_ += incoming;
      cb(ExternalBufferStatus::Ok);
    });
    return;
  }

  // Threshold crossed: migrate to a file-backed buffer. Hand it the buffered head
  // plus the frame that crossed the threshold as a single write; subsequent writes
  // and reads forward to it. The head is already counted in length_, so only the
  // crossing frame (`incoming`) is added once the write is durable.
  ENVOY_LOG(debug, "ai_protocol_manager: spilling buffer to disk at {} bytes", length_ + incoming);
  spilled_ = true;
  file_ = std::make_unique<FilesystemExternalBuffer>(manager_, path_, dispatcher_);
  auto combined = std::make_unique<Buffer::OwnedImpl>();
  combined->move(memory_); // buffered head (frees the memory tier)
  combined->move(*data);   // the frame that crossed the threshold
  writeToFile(std::move(combined), incoming, std::move(cb));
}

void TieredExternalBuffer::writeToFile(Buffer::InstancePtr data, uint64_t new_bytes,
                                       WriteCallback cb) {
  file_->write(std::move(data), [this, alive = alive_, new_bytes,
                                 cb = std::move(cb)](ExternalBufferStatus status) mutable {
    if (!*alive) {
      return;
    }
    if (status == ExternalBufferStatus::Ok) {
      length_ += new_bytes;
    }
    cb(status);
  });
}

void TieredExternalBuffer::read(uint64_t offset, uint64_t length, ReadCallback cb) {
  if (spilled_) {
    file_->read(offset, length, std::move(cb));
    return;
  }
  // Serve from memory (reads happen only during replay, after every write is
  // durable, so the tier is stable and the range lies within length_).
  ASSERT(offset + length <= length_);
  auto out = std::make_unique<Buffer::OwnedImpl>();
  if (length > 0) {
    // Copy the range straight into the output buffer's reserved storage: a single
    // copy, no intermediate scratch allocation.
    auto reservation = out->reserveSingleSlice(length);
    memory_.copyOut(offset, length, reservation.slice().mem_);
    reservation.commit(length);
  }
  cb(ExternalBufferStatus::Ok, std::move(out));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
