#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include <memory>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
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

Http::FilterHeadersStatus AiProtocolManagerFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                                 bool end_stream) {
  // A headers-only request carries no payload to inspect, so there is nothing to
  // hold the chain for: let the headers flow. (Pausing here would also deadlock,
  // since no body would ever arrive to drive the replay that releases them.)
  if (end_stream) {
    return Http::FilterHeadersStatus::Continue;
  }

  // HTTP methods that do not carry request payload semantics (GET, HEAD, OPTIONS, DELETE)
  // should pass through directly without holding headers or attempting JSON parsing.
  const absl::string_view method = headers.getMethodValue();
  const auto& method_values = Http::Headers::get().MethodValues;
  if (method == method_values.Get || method == method_values.Head ||
      method == method_values.Options || method == method_values.Delete) {
    return Http::FilterHeadersStatus::Continue;
  }

  // Resolve route-specific configuration.
  const auto* per_route =
      Http::Utility::resolveMostSpecificPerFilterConfig<AiProtocolManagerPerRouteConfig>(
          decoder_callbacks_);
  if (per_route != nullptr) {
    should_parse_ = true;
    strict_parsing_ = true;
    target_schema_ = per_route->targetSchema();
    normalize_ = per_route->normalize();
  } else if (config_ != nullptr && config_->bestEffortParsing()) {
    should_parse_ = true;
    strict_parsing_ = false;
  } else {
    should_parse_ = false;
    strict_parsing_ = false;
  }

  if (!should_parse_) {
    return Http::FilterHeadersStatus::Continue;
  }

  JsonWithExtBufParserConfig parser_config;
  parser_config.should_offload_key = [](absl::string_view key, int /*depth*/) {
    return key == "content";
  };
  json_parser_.emplace(std::move(parser_config));
  parsed_doc_.reset();
  parsing_failed_ = false;

  // A body follows: pin the headers at this filter so the subsequent routing and
  // admission filters do not act on them until the payload has been offloaded.
  // decodeData() still fires on this filter while iteration is stopped here, so
  // the BufferManager keeps offloading; the held headers are released when replay
  // injects the first body frame (or, for an empty/trailer-only body, when the
  // BufferManager continues iteration).
  ENVOY_LOG(trace, "ai_protocol_manager: holding headers until payload is offloaded");
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus AiProtocolManagerFilter::decodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  if (!should_parse_) {
    return Http::FilterDataStatus::Continue;
  }

  if (parsing_failed_ && strict_parsing_) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (json_parser_.has_value() && !parsing_failed_) {
    for (const Buffer::RawSlice& slice : data.getRawSlices()) {
      if (slice.len_ == 0) {
        continue;
      }
      absl::string_view chunk(static_cast<const char*>(slice.mem_), slice.len_);
      absl::Status status = json_parser_->feed(chunk, /*is_last=*/false);
      if (!status.ok()) {
        parsing_failed_ = true;
        if (strict_parsing_) {
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
    if (!finalizeParsing()) {
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
    replayBufferedBody(/*from_trailers=*/false);
  }

  // Hold the chain here; the BufferManager replays the payload once told to.
  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap&) {
  if (!should_parse_) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (parsing_failed_ && strict_parsing_) {
    return Http::FilterTrailersStatus::StopIteration;
  }

  // A trailer-only request (no body) has nothing to replay; let the trailers flow.
  if (decode_manager_->empty()) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (!finalizeParsing()) {
    return Http::FilterTrailersStatus::StopIteration;
  }

  // Hold the trailers behind the replayed body until the replay-done callback
  // above releases them.
  replayBufferedBody(/*from_trailers=*/true);
  return Http::FilterTrailersStatus::StopIteration;
}

bool AiProtocolManagerFilter::finalizeParsing() {
  if (!json_parser_.has_value() || parsing_failed_) {
    return !strict_parsing_ || !parsing_failed_;
  }

  absl::Status status = json_parser_->feed("", /*is_last=*/true);
  if (!status.ok()) {
    parsing_failed_ = true;
    if (strict_parsing_) {
      ENVOY_LOG(debug, "ai_protocol_manager: JSON parse error on end_stream: {}", status.message());
      decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                         absl::StrCat("Invalid JSON payload: ", status.message()),
                                         nullptr, absl::nullopt, "bad_json_payload");
      return false;
    }
    ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON parse error on end_stream: {}",
              status.message());
  } else {
    auto doc_or = json_parser_->finalize();
    if (!doc_or.ok()) {
      parsing_failed_ = true;
      if (strict_parsing_) {
        ENVOY_LOG(debug, "ai_protocol_manager: JSON finalize error: {}", doc_or.status().message());
        decoder_callbacks_->sendLocalReply(
            Http::Code::BadRequest,
            absl::StrCat("Invalid JSON payload: ", doc_or.status().message()), nullptr,
            absl::nullopt, "bad_json_payload");
        return false;
      }
      ENVOY_LOG(debug, "ai_protocol_manager: best-effort JSON finalize error: {}",
                doc_or.status().message());
    } else {
      parsed_doc_ = std::move(*doc_or);
    }
  }
  return true;
}

void AiProtocolManagerFilter::replayBufferedBody(bool from_trailers) {
  decode_manager_->endStream();
  decode_manager_->replay(0, decode_manager_->length(), [this, from_trailers]() {
    if (from_trailers) {
      // Body fully replayed; release the held trailers (they carry END_STREAM) so
      // they follow the body in order.
      decoder_callbacks_->continueDecoding();
    } else {
      // Terminate the stream with an empty end_stream data frame after the replayed
      // body (also releases the held headers when the body was empty).
      Buffer::OwnedImpl end_marker;
      decoder_callbacks_->injectDecodedDataToFilterChain(end_marker, /*end_stream=*/true);
    }
  });
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
