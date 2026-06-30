#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

BufferManager::BufferManager(ExternalBufferPtr buffer, Callbacks& callbacks, uint64_t chunk_size,
                             uint64_t buffer_limit)
    : buffer_(std::move(buffer)), callbacks_(callbacks), chunk_size_(chunk_size),
      write_queue_([this]() { callbacks_.resumeSource(); }, [this]() { callbacks_.pauseSource(); },
                   []() {}) {
  write_queue_.setWatermarks(buffer_limit);
}

BufferManager::~BufferManager() { *destroyed_ = true; }

void BufferManager::registerWatermarkCallbacks(
    std::function<void(Http::UpstreamWatermarkCallbacks&)> register_fn,
    std::function<void(Http::UpstreamWatermarkCallbacks&)> unregister_fn) {
  register_fn(*this);
  unregister_fn_ = unregister_fn;
}

void BufferManager::onDestroy() {
  *destroyed_ = true;
  if (unregister_fn_) {
    unregister_fn_(*this);
    unregister_fn_ = nullptr;
  }
}

Http::FilterDataStatus BufferManager::onData(Buffer::Instance& data, bool end_stream) {
  seen_end_stream_ = end_stream;

  if (data.length() > 0) {
    write_queue_.move(data);
  }

  maybeTriggerWrite();

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

void BufferManager::setEndStream(bool has_trailers) {
  seen_end_stream_ = true;
  has_trailers_ = has_trailers;
  maybeTriggerWrite();
}

void BufferManager::maybeTriggerWrite() {
  if (pending_write_) {
    return;
  }

  if (write_queue_.length() == 0) {
    if (seen_end_stream_) {
      startReadingBack();
    }
    return;
  }

  if (seen_end_stream_ || write_queue_.highWatermarkTriggered()) {
    triggerWrite();
  }
}

void BufferManager::triggerWrite() {
  ASSERT(!pending_write_);
  ASSERT(write_queue_.length() > 0);

  pending_write_ = true;

  active_write_chunk_ = std::make_unique<Buffer::OwnedImpl>();
  active_write_chunk_->move(write_queue_);

  buffer_->write(*active_write_chunk_, *this);
}

void BufferManager::onWriteComplete(absl::Status status) {
  auto destroyed = destroyed_;
  callbacks_.dispatcher().post([this, destroyed, status]() {
    if (*destroyed) {
      return;
    }
    onWriteCompleteInternal(status);
  });
}

void BufferManager::onWriteCompleteInternal(absl::Status status) {
  pending_write_ = false;
  active_write_chunk_.reset();

  if (!status.ok()) {
    callbacks_.onFailure(status);
    return;
  }

  maybeTriggerWrite();
}

void BufferManager::startReadingBack() {
  reading_back_ = true;
  read_offset_ = 0;
  total_size_ = buffer_->size();

  if (total_size_ == 0) {
    Buffer::OwnedImpl empty;
    const bool end_stream = seen_end_stream_ && !has_trailers_;
    callbacks_.injectData(empty, end_stream);
    reading_back_ = false;
    callbacks_.onDecodingComplete();
    return;
  }

  if (sink_backed_up_) {
    return;
  }

  readNextChunk();
}

void BufferManager::readNextChunk() {
  uint64_t remaining = total_size_ - read_offset_;
  uint64_t to_read = std::min(remaining, chunk_size_);

  if (to_read == 0) {
    reading_back_ = false;
    return;
  }

  buffer_->read(read_offset_, to_read, *this);
}

void BufferManager::onReadComplete(absl::StatusOr<Buffer::InstancePtr> data) {
  auto destroyed = destroyed_;
  callbacks_.dispatcher().post([this, destroyed, data = std::move(data)]() mutable {
    if (*destroyed) {
      return;
    }
    onReadCompleteInternal(std::move(data));
  });
}

void BufferManager::onReadCompleteInternal(absl::StatusOr<Buffer::InstancePtr> data) {
  if (!data.ok()) {
    reading_back_ = false;
    callbacks_.onFailure(data.status());
    return;
  }

  Buffer::InstancePtr chunk = std::move(data.value());
  uint64_t chunk_len = chunk->length();
  read_offset_ += chunk_len;

  const bool is_last_chunk = (read_offset_ == total_size_);
  const bool end_stream = is_last_chunk && !has_trailers_;

  callbacks_.injectData(*chunk, end_stream);

  if (is_last_chunk) {
    reading_back_ = false;
    callbacks_.onDecodingComplete();
  } else {
    if (sink_backed_up_) {
      return;
    }
    readNextChunk();
  }
}

void BufferManager::onSinkHighWatermark() { sink_backed_up_ = true; }

void BufferManager::onSinkLowWatermark() {
  sink_backed_up_ = false;
  if (reading_back_) {
    readNextChunk();
  }
}

void BufferManager::onAboveWriteBufferHighWatermark() { onSinkHighWatermark(); }

void BufferManager::onBelowWriteBufferLowWatermark() { onSinkLowWatermark(); }

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
