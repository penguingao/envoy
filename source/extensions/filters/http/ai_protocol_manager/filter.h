#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/router/router.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/ai/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/ai_filter_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_emitter.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf_parser.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using PerRouteProto =
    envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute;

// Compiled AI filter chain: one factory per configured filter, built once at
// config time and instantiated per stream.
using AiFilterFactoryCbs = std::vector<AiFilters::AiFilterFactoryCb>;

// Compiles `proto` into per-stream factories, failing if a configured filter is
// not a registered envoy.filters.ai extension.
absl::StatusOr<AiFilterFactoryCbs> compileAiFilters(
    const envoy::extensions::filters::http::ai_protocol_manager::v3::AiFilterChain& proto,
    const std::string& stats_prefix, Server::Configuration::ServerFactoryContext& context);

// Filter-level configuration, shared by every stream on the chain.
class FilterConfig {
public:
  FilterConfig(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager& proto,
      AiFilterFactoryCbs ai_filters)
      : best_effort_parsing_(proto.best_effort_parsing()), ai_filters_(std::move(ai_filters)) {}

  bool bestEffortParsing() const { return best_effort_parsing_; }
  const AiFilterFactoryCbs& aiFilters() const { return ai_filters_; }

private:
  const bool best_effort_parsing_;
  const AiFilterFactoryCbs ai_filters_;
};
using FilterConfigSharedPtr = std::shared_ptr<const FilterConfig>;

// Per-route configuration. Its presence declares the route an AI endpoint: the
// payload is parsed strictly, validated against schema(), and transcoded to the
// canonical schema when normalize() is set.
class RouteConfig : public Router::RouteSpecificFilterConfig {
public:
  RouteConfig(const PerRouteProto& proto, std::optional<AiFilterFactoryCbs> ai_filters)
      : schema_(proto.schema()), normalize_(proto.normalize()), ai_filters_(std::move(ai_filters)) {
  }

  PerRouteProto::Schema schema() const { return schema_; }
  bool normalize() const { return normalize_; }

  // Engaged when the route configured a chain, in which case it replaces the
  // filter-level one rather than adding to it.
  const std::optional<AiFilterFactoryCbs>& aiFilters() const { return ai_filters_; }

private:
  const PerRouteProto::Schema schema_;
  const bool normalize_;
  const std::optional<AiFilterFactoryCbs> ai_filters_;
};

// AI Protocol Manager HTTP filter (alpha).
//
// The filter manages AI endpoint traffic: it holds a request payload, validates
// it against the schema the endpoint serves, and normalizes it to a canonical
// schema -- which is what lets routing, admission and policy act on a payload
// the proxy understands rather than on opaque bytes.
//
// As the body arrives the filter offloads it into an ExternalBuffer -- keeping
// a large payload out of the connection manager's buffers -- and parses and
// validates the JSON in a streaming fashion alongside. The chain is held
// meanwhile: decodeHeaders() stops iteration when a body follows, and the
// headers stay pinned here while decodeData() keeps offloading. Only once the
// payload is validated does the filter replay the buffered body back into the
// chain; the first injectDecodedDataToFilterChain() call releases the held
// headers ahead of it, so subsequent filters see the headers immediately
// followed by the payload. An invalid payload is rejected rather than
// forwarded.
//
// None of that happens for a stream the filter has no reason to inspect:
// decodeHeaders() returns Continue and the offload path is never entered.
//
// The offload/replay pipeline and its bidirectional flow control live in the
// path-agnostic BufferManager (buffer_manager.h); the filter is a thin delegator
// that constructs one BufferManager per direction with the matching
// FilterChainBridge (filter_chain_bridge.h). Today only the decode (request) path
// is wired; the encode path will construct a second BufferManager with the
// encoder bridge.
//
// Parsing runs alongside the offload: every body frame is fed to a
// JsonWithExtBufParser before it reaches the BufferManager, so the two see the
// identical byte stream from the first body byte -- which is what makes the
// parser's recorded offsets valid buffer offsets (json_with_ext_buf_parser.h).
// Feeding first also fails a malformed payload the moment the bad byte arrives,
// not after the whole upload.
//
// A route carrying a RouteConfig is a declared AI endpoint, and its payload is
// the filter's to manage: parsed strictly, with a malformed one rejected so
// Envoy and the backend cannot read the same body differently. A route without
// one is parsed only if the filter was configured for best effort -- offered for
// compatibility with chains that want a parsed body on ordinary routes, never a
// reason to fail a request -- and is otherwise untouched.
//
// The schema is not acted on yet: validation against it and transcoding to the
// canonical schema when the route asks to normalize both come later.
class AiProtocolManagerFilter : public Http::PassThroughFilter,
                                public Logger::Loggable<Logger::Id::filter> {
public:
  AiProtocolManagerFilter(ExternalBufferFactory& buffer_factory, FilterConfigSharedPtr config)
      : buffer_factory_(buffer_factory), config_(std::move(config)) {}

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;

private:
  // Bridges the AI filter callbacks onto this filter's HTTP callbacks. Headers
  // are still held here, so a header an AI filter sets is seen by routing.
  class ChainCallbacks : public AiFilters::AiFilterCallbacks {
  public:
    explicit ChainCallbacks(AiProtocolManagerFilter& parent) : parent_(parent) {}

    Http::RequestHeaderMapOptRef requestHeaders() override {
      return parent_.decoder_callbacks_->requestHeaders();
    }
    void sendLocalReply(Http::Code code, absl::string_view body,
                        absl::string_view details) override {
      parent_.payload_rejected_ = true;
      parent_.decoder_callbacks_->sendLocalReply(code, body, nullptr, std::nullopt, details);
    }
    StreamInfo::StreamInfo& streamInfo() override {
      return parent_.decoder_callbacks_->streamInfo();
    }
    Event::Dispatcher& dispatcher() override { return parent_.decoder_callbacks_->dispatcher(); }

  private:
    AiProtocolManagerFilter& parent_;
  };

  // Adapts the emitter onto the buffer manager's source interface. Kept here
  // rather than in json_emitter.h so the emitter stays independent of the
  // buffer manager and remains testable on its own.
  class JsonEmitterSource : public EmitSource {
  public:
    explicit JsonEmitterSource(JsonEmitter& emitter) : emitter_(emitter) {}

    Piece next() override {
      switch (emitter_.next()) {
      case JsonEmitter::State::Text:
        return Piece::Text;
      case JsonEmitter::State::Range:
        return Piece::Range;
      case JsonEmitter::State::Done:
        return Piece::Done;
      }
      return Piece::Done;
    }
    absl::string_view text() const override { return emitter_.text(); }
    uint64_t rangeOffset() const override { return emitter_.range().offset; }
    uint64_t rangeLength() const override { return emitter_.range().length; }
    absl::Status status() const override { return emitter_.status(); }

  private:
    JsonEmitter& emitter_;
  };

  // Delivers an emitted payload into the filter chain.
  class ChainEmitSink : public ReplaySink {
  public:
    explicit ChainEmitSink(AiProtocolManagerFilter& parent) : parent_(parent) {}

    Disposition onReplayData(Buffer::Instance& data) override;
    void onReplayComplete() override;

  private:
    AiProtocolManagerFilter& parent_;
  };

  // Builds and runs the AI filter chain over the parsed payload. Returns false
  // if the request was terminated.
  bool startAiFilterChain();

  // Ends the forwarded payload the way this stream requires.
  void finishForwarding();

  // Forwards the payload downstream once the chain is done: re-serialized when a
  // filter modified it, replayed verbatim when nobody did.
  void forwardPayload();

  // Feeds one body frame to the parser in place. Returns false only if the
  // payload was rejected, in which case the caller must not offload or replay
  // it; a best-effort parse that fails abandons parsing and returns true.
  bool feedParser(const Buffer::Instance& data, bool end_stream);

  // Terminates the stream with a 400 for a payload that failed to parse.
  void rejectInvalidPayload(const absl::Status& status);

  // Whether the route declared itself an AI endpoint, which is also what makes a
  // parse failure fatal.
  bool isAiEndpoint() const { return schema_ != PerRouteProto::UNSPECIFIED; }

  ExternalBufferFactory& buffer_factory_;
  FilterConfigSharedPtr config_;

  // Non-null exactly when decodeHeaders() decided to inspect this stream, so it
  // doubles as the engaged flag. Outlives request_parser_, which is released as
  // soon as parsing is done with.
  BufferManagerPtr decode_manager_;

  // Copied out of the route configuration rather than held by pointer: the route
  // can be re-resolved mid-stream, which would leave a cached pointer dangling,
  // and these are two scalars.
  PerRouteProto::Schema schema_{PerRouteProto::UNSPECIFIED};
  bool normalize_{false};

  // The parsed payload. Populated once the body has been fully received and
  // parsed; nothing consumes it yet.
  JsonWithExtBuf request_json_;
  // Cleared once parsing is done with, whether it completed, was abandoned, or
  // failed the request.
  std::unique_ptr<JsonWithExtBufParser> request_parser_;

  // Once set, later frames on the dying stream are dropped, not offloaded.
  bool payload_rejected_{false};

  // The AI filter chain and what it needs, alive for the stream. Null when the
  // route configured no filters.
  ChainCallbacks chain_callbacks_{*this};
  AiFilterChainPtr ai_chain_;
  // The payload the chain produced, held until it has been forwarded.
  InferenceRequestPtr chain_result_;
  // True when trailers carried end-of-stream, so the body must be released with
  // continueDecoding() rather than a terminal data frame.
  bool trailers_pending_{false};
  // Emission state, live only while a modified payload is being serialized.
  std::unique_ptr<JsonEmitter> emitter_;
  std::unique_ptr<JsonEmitterSource> emit_source_;
  ChainEmitSink emit_sink_{*this};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
