#include "source/extensions/filters/http/ai_protocol_manager/ai_protocol_manager.h"

#include "source/extensions/filters/http/ai_protocol_manager/in_memory_buffer.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

AiProtocolManagerFilter::AiProtocolManagerFilter() : AiProtocolManagerFilter(1024 * 1024) {}

AiProtocolManagerFilter::AiProtocolManagerFilter(uint64_t buffer_limit)
    : buffer_limit_(buffer_limit) {
  decode_buffer_manager_ = std::make_unique<BufferManager>(
      std::make_unique<InMemoryBuffer>(), decode_callbacks_, chunk_size_, buffer_limit_);
}

AiProtocolManagerFilter::~AiProtocolManagerFilter() = default;

void AiProtocolManagerFilter::onDestroy() {
  if (decode_buffer_manager_) {
    decode_buffer_manager_->onDestroy();
  }
  Http::PassThroughFilter::onDestroy();
}

void AiProtocolManagerFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  Http::PassThroughFilter::setDecoderFilterCallbacks(callbacks);
  decode_buffer_manager_->registerWatermarkCallbacks(
      [&callbacks](Http::UpstreamWatermarkCallbacks& cb) {
        callbacks.addUpstreamWatermarkCallbacks(cb);
      },
      [&callbacks](Http::UpstreamWatermarkCallbacks& cb) {
        callbacks.removeUpstreamWatermarkCallbacks(cb);
      });
}

Http::FilterHeadersStatus AiProtocolManagerFilter::decodeHeaders(Http::RequestHeaderMap&,
                                                                 bool end_stream) {
  if (end_stream) {
    return Http::FilterHeadersStatus::Continue;
  }
  headers_paused_ = true;
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus AiProtocolManagerFilter::decodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  return decode_buffer_manager_->onData(data, end_stream);
}

Http::FilterTrailersStatus AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap&) {
  has_trailers_ = true;
  decode_buffer_manager_->setEndStream(true);
  return Http::FilterTrailersStatus::StopIteration;
}

void AiProtocolManagerFilter::DecodeCallbacks::pauseSource() {
  filter_.decoder_callbacks_->onDecoderFilterAboveWriteBufferHighWatermark();
}

void AiProtocolManagerFilter::DecodeCallbacks::resumeSource() {
  filter_.decoder_callbacks_->onDecoderFilterBelowWriteBufferLowWatermark();
}

void AiProtocolManagerFilter::DecodeCallbacks::injectData(Buffer::Instance& data, bool end_stream) {
  filter_.decoder_callbacks_->injectDecodedDataToFilterChain(data, end_stream);
}

void AiProtocolManagerFilter::DecodeCallbacks::onDecodingComplete() {
  if (filter_.has_trailers_) {
    filter_.decoder_callbacks_->continueDecoding();
  }
}

void AiProtocolManagerFilter::DecodeCallbacks::onFailure(absl::Status status) {
  filter_.decoder_callbacks_->sendLocalReply(
      Http::Code::InternalServerError, absl::StrCat("External buffer error: ", status.message()),
      nullptr, std::nullopt, "");
}

Event::Dispatcher& AiProtocolManagerFilter::DecodeCallbacks::dispatcher() {
  return filter_.decoder_callbacks_->dispatcher();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
