#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/utility.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_chain_bridge.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

bool AiProtocolManagerFilter::shouldParseJson() const {
  // If a route-specific config is matched, parse the JSON payload according to the route config.
  if (route_config_ != nullptr) {
    return true;
  }
  // If best-effort parsing is configured at the filter level without route config, parse JSON.
  if (config_ != nullptr && config_->bestEffortParsing()) {
    return true;
  }
  // Default behavior: do not parse JSON, just pass through the payload.
  return false;
}

bool AiProtocolManagerFilter::shouldFailOnBadJson() const {
  // Only fail the request on bad JSON when a route-specific config is present.
  return route_config_ != nullptr;
}

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
  route_config_ = nullptr;
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

  // Resolve route-specific configuration if present.
  route_config_ =
      Http::Utility::resolveMostSpecificPerFilterConfig<AiProtocolManagerPerRouteConfig>(
          decoder_callbacks_);

  // If not parsing (default behavior when no route config matches and best-effort parsing
  // is disabled), do not hold the chain or offload the payload: let headers and data flow.
  if (!shouldParseJson()) {
    return Http::FilterHeadersStatus::Continue;
  }

  // A body follows and will be parsed: pin the headers at this filter so the subsequent
  // routing and admission filters do not act on them until the payload has been offloaded.
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
  if (json_parser_ == nullptr && !parsing_failed_) {
    if (decoder_callbacks_ != nullptr && route_config_ == nullptr) {
      route_config_ =
          Http::Utility::resolveMostSpecificPerFilterConfig<AiProtocolManagerPerRouteConfig>(
              decoder_callbacks_);
    }
    if (shouldParseJson()) {
      json_parser_ = std::make_unique<JsonWithExtBufParser>(
          /*ext_buf=*/nullptr, [](absl::string_view key, int /*depth*/, size_t /*token_start*/) {
            return key == "content";
          });
      parsed_doc_.reset();
      parsing_failed_ = false;
    }
  }

  // If not parsing, pass through data without offloading.
  if (!shouldParseJson()) {
    return Http::FilterDataStatus::Continue;
  }

  if (parsing_failed_ && shouldFailOnBadJson()) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (json_parser_ != nullptr && !parsing_failed_) {
    for (const Buffer::RawSlice& slice : data.getRawSlices()) {
      if (slice.len_ == 0) {
        continue;
      }
      absl::string_view chunk(static_cast<const char*>(slice.mem_), slice.len_);
      absl::Status status = json_parser_->feed(chunk, /*is_last=*/false);
      if (!status.ok()) {
        parsing_failed_ = true;
        if (shouldFailOnBadJson()) {
          ENVOY_LOG(debug, "ai_protocol_manager: JSON parse error: {}", status.message());
          decoder_callbacks_->sendLocalReply(
              Http::Code::BadRequest, absl::StrCat("Invalid JSON payload: ", status.message()),
              nullptr, absl::nullopt, "bad_json_payload");
          return Http::FilterDataStatus::StopIterationNoBuffer;
        }
        ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON parse error: {}; passing through",
                  status.message());
        break;
      }
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

    if (json_parser_ != nullptr && !parsing_failed_) {
      absl::Status status = json_parser_->feed("", /*is_last=*/true);
      if (!status.ok()) {
        parsing_failed_ = true;
        if (shouldFailOnBadJson()) {
          ENVOY_LOG(debug, "ai_protocol_manager: JSON parse error on end_stream: {}",
                    status.message());
          decoder_callbacks_->sendLocalReply(
              Http::Code::BadRequest, absl::StrCat("Invalid JSON payload: ", status.message()),
              nullptr, absl::nullopt, "bad_json_payload");
          return Http::FilterDataStatus::StopIterationNoBuffer;
        }
        ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON parse error on end_stream: {}",
                  status.message());
      } else {
        auto doc_or = json_parser_->finalize();
        if (!doc_or.ok()) {
          parsing_failed_ = true;
          if (shouldFailOnBadJson()) {
            ENVOY_LOG(debug, "ai_protocol_manager: JSON finalize error: {}",
                      doc_or.status().message());
            decoder_callbacks_->sendLocalReply(
                Http::Code::BadRequest,
                absl::StrCat("Invalid JSON payload: ", doc_or.status().message()), nullptr,
                absl::nullopt, "bad_json_payload");
            return Http::FilterDataStatus::StopIterationNoBuffer;
          }
          ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON finalize error: {}",
                    doc_or.status().message());
        } else {
          parsed_doc_ = std::move(*doc_or);
        }
      }
    }

    decode_manager_->endStream();
    decode_manager_->replay(0, decode_manager_->length(), [this]() {
      Buffer::OwnedImpl end_marker;
      decoder_callbacks_->injectDecodedDataToFilterChain(end_marker, /*end_stream=*/true);
    });
  }

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap&) {
  if (!shouldParseJson()) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (parsing_failed_ && shouldFailOnBadJson()) {
    return Http::FilterTrailersStatus::StopIteration;
  }

  // A trailer-only request (no body) has nothing to replay; let the trailers flow.
  if (decode_manager_->empty()) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (json_parser_ != nullptr && !parsed_doc_ && !parsing_failed_) {
    absl::Status status = json_parser_->feed("", /*is_last=*/true);
    if (!status.ok()) {
      parsing_failed_ = true;
      if (shouldFailOnBadJson()) {
        decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                           absl::StrCat("Invalid JSON payload: ", status.message()),
                                           nullptr, absl::nullopt, "bad_json_payload");
        return Http::FilterTrailersStatus::StopIteration;
      }
      ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON parse error on trailers: {}",
                status.message());
    } else {
      auto doc_or = json_parser_->finalize();
      if (!doc_or.ok()) {
        parsing_failed_ = true;
        if (shouldFailOnBadJson()) {
          decoder_callbacks_->sendLocalReply(
              Http::Code::BadRequest,
              absl::StrCat("Invalid JSON payload: ", doc_or.status().message()), nullptr,
              absl::nullopt, "bad_json_payload");
          return Http::FilterTrailersStatus::StopIteration;
        }
        ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON finalize error on trailers: {}",
                  doc_or.status().message());
      } else {
        parsed_doc_ = std::move(*doc_or);
      }
    }
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
