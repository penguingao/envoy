#include "source/extensions/filters/http/ai_protocol_manager/ai_protocol_manager.h"

#include "source/extensions/filters/http/ai_protocol_manager/in_memory_buffer.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

AiProtocolManagerFilter::AiProtocolManagerFilter() { buffer_ = std::make_unique<InMemoryBuffer>(); }

AiProtocolManagerFilter::~AiProtocolManagerFilter() { *destroyed_ = true; }

void AiProtocolManagerFilter::onDestroy() {
  if (decoder_callbacks_) {
    decoder_callbacks_->removeUpstreamWatermarkCallbacks(*this);
  }
  Http::PassThroughFilter::onDestroy();
}

void AiProtocolManagerFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  Http::PassThroughFilter::setDecoderFilterCallbacks(callbacks);
  decoder_callbacks_->addUpstreamWatermarkCallbacks(*this);
}

void AiProtocolManagerFilter::onAboveWriteBufferHighWatermark() { upstream_backpressured_ = true; }

void AiProtocolManagerFilter::onBelowWriteBufferLowWatermark() {
  upstream_backpressured_ = false;
  if (active_read_callback_ && read_offset_ < total_size_) {
    readNextChunk();
  }
}

void AiProtocolManagerFilter::WriteCallback::onWriteComplete(absl::Status status) {
  auto destroyed = filter_.destroyed_;
  filter_.decoder_callbacks_->dispatcher().post([filter = &filter_, destroyed, status]() {
    if (*destroyed) {
      return;
    }
    filter->onWriteComplete(status);
  });
}

void AiProtocolManagerFilter::ReadCallback::onReadComplete(
    absl::StatusOr<Buffer::InstancePtr> data) {
  auto destroyed = filter_.destroyed_;
  filter_.decoder_callbacks_->dispatcher().post(
      [filter = &filter_, destroyed, data = std::move(data)]() mutable {
        if (*destroyed) {
          return;
        }
        filter->onReadComplete(std::move(data));
      });
}

Http::FilterDataStatus AiProtocolManagerFilter::decodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  seen_end_stream_ = end_stream;

  if (data.length() > 0) {
    auto chunk = std::make_unique<Buffer::OwnedImpl>();
    chunk->move(data);

    pending_write_ = true;
    active_write_callback_ = std::make_shared<WriteCallback>(*this, std::move(chunk));

    // Manually signal high watermark to pause downstream (client) reads.
    // This bounds the memory because the client will be stopped from sending more data
    // while we are writing this chunk to the external buffer.
    decoder_callbacks_->onDecoderFilterAboveWriteBufferHighWatermark();

    buffer_->write(*active_write_callback_->chunk_, *active_write_callback_);
  } else if (end_stream) {
    startReadingBack();
  }

  // Use StopIterationNoBuffer because we have drained the data and offloaded it.
  // We don't want Envoy to buffer anything.
  return Http::FilterDataStatus::StopIterationNoBuffer;
}

void AiProtocolManagerFilter::onWriteComplete(absl::Status status) {
  active_write_callback_.reset();
  pending_write_ = false;

  // Manually signal low watermark to resume downstream (client) reads.
  decoder_callbacks_->onDecoderFilterBelowWriteBufferLowWatermark();

  if (!status.ok()) {
    decoder_callbacks_->sendLocalReply(
        Http::Code::InternalServerError,
        absl::StrCat("External buffer write failed: ", status.message()), nullptr, std::nullopt,
        "");
    return;
  }

  if (seen_end_stream_) {
    startReadingBack();
  } else {
    decoder_callbacks_->continueDecoding();
  }
}

void AiProtocolManagerFilter::startReadingBack() {
  read_offset_ = 0;
  total_size_ = buffer_->size();

  if (total_size_ == 0) {
    Buffer::OwnedImpl empty;
    decoder_callbacks_->injectDecodedDataToFilterChain(empty, true);
    return;
  }

  active_read_callback_ = std::make_shared<ReadCallback>(*this);

  if (upstream_backpressured_) {
    return;
  }

  readNextChunk();
}

void AiProtocolManagerFilter::readNextChunk() {
  uint64_t remaining = total_size_ - read_offset_;
  uint64_t to_read = std::min(remaining, chunk_size_);

  if (to_read == 0) {
    active_read_callback_.reset();
    return;
  }

  buffer_->read(read_offset_, to_read, *active_read_callback_);
}

void AiProtocolManagerFilter::onReadComplete(absl::StatusOr<Buffer::InstancePtr> data) {
  if (!data.ok()) {
    active_read_callback_.reset();
    decoder_callbacks_->sendLocalReply(
        Http::Code::InternalServerError,
        absl::StrCat("External buffer read failed: ", data.status().message()), nullptr,
        std::nullopt, "");
    return;
  }

  Buffer::InstancePtr chunk = std::move(data.value());
  uint64_t chunk_len = chunk->length();
  read_offset_ += chunk_len;

  bool is_end_stream = (read_offset_ == total_size_);

  decoder_callbacks_->injectDecodedDataToFilterChain(*chunk, is_end_stream);

  if (!is_end_stream) {
    if (upstream_backpressured_) {
      return;
    }
    readNextChunk();
  } else {
    active_read_callback_.reset();
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
