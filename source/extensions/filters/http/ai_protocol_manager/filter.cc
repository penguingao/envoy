#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_chain_bridge.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

void AiProtocolManagerFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  PassThroughFilter::setDecoderFilterCallbacks(callbacks);
  // Construct the decode-path manager. Its constructor subscribes to upstream
  // watermarks (via the bridge) so replay can be paced against upstream
  // back-pressure; subscribing may immediately deliver high-watermark callbacks
  // if the upstream is already backed up.
  decode_manager_ = std::make_unique<BufferManager>(
      buffer_factory_, std::make_unique<DecoderFilterChainBridge>(*decoder_callbacks_));
}

void AiProtocolManagerFilter::onDestroy() {
  json_parser_.reset();
  parsed_doc_.reset();
  if (decode_manager_ != nullptr) {
    // Detach the manager (releases the external buffer and unsubscribes from
    // watermarks) but do NOT free it here. onDestroy() can run synchronously while
    // the manager is mid-replay -- a downstream filter answering an injected frame
    // with a local reply reaches destroyFilters() on this very stack -- and freeing
    // the manager then would pull it out from under its own injectData()/read()
    // reentrancy. The manager is owned by unique_ptr and freed when this filter is
    // (deferred-)destroyed, by which point the replay stack has unwound. This honors
    // BufferManager's onDestroy()-before-destruction contract (see buffer_manager.h).
    decode_manager_->onDestroy();
  }
}

Http::FilterHeadersStatus AiProtocolManagerFilter::decodeHeaders(Http::RequestHeaderMap&,
                                                                 bool end_stream) {
  // A headers-only request carries no payload to inspect, so there is nothing to
  // hold the chain for: let the headers flow. (Pausing here would also deadlock,
  // since no body would ever arrive to drive the replay that releases them.)
  if (end_stream) {
    return Http::FilterHeadersStatus::Continue;
  }

  // A body follows: pin the headers at this filter so the subsequent routing and
  // admission filters do not act on them until the payload has been offloaded.
  // decodeData() still fires on this filter while iteration is stopped here, so
  // the BufferManager keeps offloading; the held headers are released when replay
  // injects the first body frame (or, for an empty/trailer-only body, when the
  // BufferManager continues iteration).
  json_parser_ = std::make_unique<JsonWithExtBufParser>(
      /*ext_buf=*/nullptr, [](absl::string_view key, int /*depth*/, size_t /*token_start*/) {
        return key == "content";
      });
  parsed_doc_.reset();
  parsing_failed_ = false;

  ENVOY_LOG(trace, "ai_protocol_manager: holding headers until payload is offloaded");
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus AiProtocolManagerFilter::decodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  if (parsing_failed_) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (json_parser_ == nullptr) {
    json_parser_ = std::make_unique<JsonWithExtBufParser>(
        /*ext_buf=*/nullptr, [](absl::string_view key, int /*depth*/, size_t /*token_start*/) {
          return key == "content";
        });
    parsed_doc_.reset();
    parsing_failed_ = false;
  }

  for (const Buffer::RawSlice& slice : data.getRawSlices()) {
    if (slice.len_ == 0) {
      continue;
    }
    absl::string_view chunk(static_cast<const char*>(slice.mem_), slice.len_);
    absl::Status status = json_parser_->feed(chunk, /*is_last=*/false);
    if (!status.ok()) {
      ENVOY_LOG(debug, "ai_protocol_manager: JSON parse error: {}", status.message());
      parsing_failed_ = true;
      decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                         absl::StrCat("Invalid JSON payload: ", status.message()),
                                         nullptr, absl::nullopt, "bad_json_payload");
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
  }

  decode_manager_->onData(data);

  if (end_stream) {
    if (decode_manager_->empty()) {
      decode_manager_->endStream();
      decode_manager_->replay(0, decode_manager_->length(), [this]() {
        Buffer::OwnedImpl end_marker;
        decoder_callbacks_->injectDecodedDataToFilterChain(end_marker, /*end_stream=*/true);
      });
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }

    absl::Status status = json_parser_->feed("", /*is_last=*/true);
    if (!status.ok()) {
      ENVOY_LOG(debug, "ai_protocol_manager: JSON parse error on end_stream: {}", status.message());
      parsing_failed_ = true;
      decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                         absl::StrCat("Invalid JSON payload: ", status.message()),
                                         nullptr, absl::nullopt, "bad_json_payload");
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
    auto doc_or = json_parser_->finalize();
    if (!doc_or.ok()) {
      ENVOY_LOG(debug, "ai_protocol_manager: JSON finalize error: {}", doc_or.status().message());
      parsing_failed_ = true;
      decoder_callbacks_->sendLocalReply(
          Http::Code::BadRequest, absl::StrCat("Invalid JSON payload: ", doc_or.status().message()),
          nullptr, absl::nullopt, "bad_json_payload");
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
    parsed_doc_ = std::move(*doc_or);

    decode_manager_->endStream();
    decode_manager_->replay(0, decode_manager_->length(), [this]() {
      Buffer::OwnedImpl end_marker;
      decoder_callbacks_->injectDecodedDataToFilterChain(end_marker, /*end_stream=*/true);
    });
  }

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap&) {
  if (parsing_failed_) {
    return Http::FilterTrailersStatus::StopIteration;
  }

  // A trailer-only request (no body) has nothing to replay; let the trailers flow.
  if (decode_manager_->empty()) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (json_parser_ != nullptr && !parsed_doc_) {
    absl::Status status = json_parser_->feed("", /*is_last=*/true);
    if (!status.ok()) {
      parsing_failed_ = true;
      decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                         absl::StrCat("Invalid JSON payload: ", status.message()),
                                         nullptr, absl::nullopt, "bad_json_payload");
      return Http::FilterTrailersStatus::StopIteration;
    }
    auto doc_or = json_parser_->finalize();
    if (!doc_or.ok()) {
      parsing_failed_ = true;
      decoder_callbacks_->sendLocalReply(
          Http::Code::BadRequest, absl::StrCat("Invalid JSON payload: ", doc_or.status().message()),
          nullptr, absl::nullopt, "bad_json_payload");
      return Http::FilterTrailersStatus::StopIteration;
    }
    parsed_doc_ = std::move(*doc_or);
  }

  // The body ended without end_stream on a data frame; the trailers carry it.
  decode_manager_->endStream();
  decode_manager_->replay(0, decode_manager_->length(), [this]() {
    // Body fully replayed; release the held trailers (they carry END_STREAM) so
    // they follow the body in order.
    decoder_callbacks_->continueDecoding();
  });
  // Hold the trailers behind the replayed body until the replay-done callback
  // above releases them.
  return Http::FilterTrailersStatus::StopIteration;
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
