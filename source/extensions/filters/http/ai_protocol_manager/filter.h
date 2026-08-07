#pragma once

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/router/router.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

/**
 * Filter-level configuration for the AI Protocol Manager filter.
 */
class AiProtocolManagerFilterConfig {
public:
  explicit AiProtocolManagerFilterConfig(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&
          proto_config)
      : best_effort_parsing_(proto_config.best_effort_parsing()) {}

  bool bestEffortParsing() const { return best_effort_parsing_; }

private:
  bool best_effort_parsing_{false};
};

using AiProtocolManagerFilterConfigSharedPtr = std::shared_ptr<AiProtocolManagerFilterConfig>;

/**
 * Route-specific configuration for the AI Protocol Manager filter.
 */
class AiProtocolManagerPerRouteConfig : public Router::RouteSpecificFilterConfig {
public:
  explicit AiProtocolManagerPerRouteConfig(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute&
          proto_config)
      : target_schema_(proto_config.target_schema()), normalize_(proto_config.normalize()) {}

  const std::string& targetSchema() const { return target_schema_; }
  bool normalize() const { return normalize_; }

private:
  std::string target_schema_;
  bool normalize_{false};
};

using AiProtocolManagerPerRouteConfigConstSharedPtr =
    std::shared_ptr<const AiProtocolManagerPerRouteConfig>;

// AI Protocol Manager HTTP filter (alpha).
//
// AI requests carry a JSON payload the filter must vet before the rest of the
// chain acts on the request. As the body arrives the filter offloads it into an
// ExternalBuffer -- keeping a large payload out of the connection manager's
// buffers -- and parses and validates the JSON in a streaming fashion alongside.
// The chain is held meanwhile: decodeHeaders() stops iteration when a body
// follows, and the headers stay pinned here while decodeData() keeps offloading.
// Only once the payload is validated does the filter replay the buffered body
// back into the chain; the first injectDecodedDataToFilterChain() call releases
// the held headers ahead of it, so subsequent filters see the headers immediately
// followed by the payload. An invalid payload is rejected rather than forwarded.
//
// The offload/replay pipeline and its bidirectional flow control live in the
// path-agnostic BufferManager (buffer_manager.h); the filter is a thin delegator
// that constructs one BufferManager per direction with the matching
// FilterChainBridge (filter_chain_bridge.h). Today only the decode (request) path
// is wired; the encode path will construct a second BufferManager with the
// encoder bridge.
class AiProtocolManagerFilter : public Http::PassThroughFilter,
                                public Logger::Loggable<Logger::Id::filter> {
public:
  explicit AiProtocolManagerFilter(ExternalBufferFactory& buffer_factory,
                                   AiProtocolManagerFilterConfigSharedPtr config = nullptr)
      : buffer_factory_(buffer_factory), config_(std::move(config)) {}

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;

  // Returns the parsed JSON document, populated once validation completes at end_stream.
  const JsonWithExtBuf* parsedDoc() const { return parsed_doc_.get(); }

  // Returns the route-specific config matched for this stream, if any.
  const AiProtocolManagerPerRouteConfig* routeConfig() const { return route_config_; }

  // Returns the filter-level config, if configured.
  const AiProtocolManagerFilterConfig* filterConfig() const { return config_.get(); }

private:
  bool shouldParseJson() const;
  bool shouldFailOnBadJson() const;

  ExternalBufferFactory& buffer_factory_;
  AiProtocolManagerFilterConfigSharedPtr config_;
  const AiProtocolManagerPerRouteConfig* route_config_{nullptr};
  BufferManagerPtr decode_manager_;
  std::unique_ptr<JsonWithExtBufParser> json_parser_;
  std::unique_ptr<JsonWithExtBuf> parsed_doc_;
  bool parsing_failed_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
