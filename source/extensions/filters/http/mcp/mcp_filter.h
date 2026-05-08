#pragma once

#include <memory>
#include <string>

#include "envoy/extensions/filters/http/mcp/v3/mcp.pb.h"
#include "envoy/http/filter.h"
#include "envoy/server/filter_config.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"

#include "source/common/common/logger.h"
#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/mcp/mcp_json_parser.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Mcp {

/**
 * All MCP filter stats. @see stats_macros.h
 */
#define MCP_FILTER_STATS(COUNTER)                                                                  \
  COUNTER(requests_rejected)                                                                       \
  COUNTER(invalid_json)                                                                            \
  COUNTER(body_too_large)

/**
 * Struct definition for MCP filter stats. @see stats_macros.h
 */
struct McpFilterStats {
  MCP_FILTER_STATS(GENERATE_COUNTER_STRUCT)
};

/**
 * Configuration for the MCP filter.
 */
class McpFilterConfig {
public:
  McpFilterConfig(const envoy::extensions::filters::http::mcp::v3::Mcp& proto_config,
                  const std::string& stats_prefix, Stats::Scope& scope);

  envoy::extensions::filters::http::mcp::v3::Mcp::TrafficMode trafficMode() const {
    return traffic_mode_;
  }

  bool shouldRejectNonMcp() const {
    return traffic_mode_ == envoy::extensions::filters::http::mcp::v3::Mcp::REJECT_NO_MCP;
  }

  bool clearRouteCache() const { return clear_route_cache_; }

  const absl::optional<
      envoy::extensions::filters::http::mcp::v3::Mcp::TraceContextPropagationConfig>&
  propagateTraceContext() const {
    return propagate_trace_context_;
  }
  const absl::optional<envoy::extensions::filters::http::mcp::v3::Mcp::BaggagePropagationConfig>&
  propagateBaggage() const {
    return propagate_baggage_;
  }

  uint32_t maxRequestBodySize() const { return max_request_body_size_; }
  const ParserConfig& parserConfig() const { return parser_config_; }
  bool shouldStoreToDynamicMetadata() const {
    return request_storage_mode_ ==
               envoy::extensions::filters::http::mcp::v3::Mcp::MODE_UNSPECIFIED ||
           request_storage_mode_ ==
               envoy::extensions::filters::http::mcp::v3::Mcp::DYNAMIC_METADATA ||
           request_storage_mode_ ==
               envoy::extensions::filters::http::mcp::v3::Mcp::DYNAMIC_METADATA_AND_FILTER_STATE;
  }
  bool shouldStoreToFilterState() const {
    return request_storage_mode_ == envoy::extensions::filters::http::mcp::v3::Mcp::FILTER_STATE ||
           request_storage_mode_ ==
               envoy::extensions::filters::http::mcp::v3::Mcp::DYNAMIC_METADATA_AND_FILTER_STATE;
  }
  const std::string& metadataNamespace() const { return metadata_namespace_; }

  McpFilterStats& stats() { return stats_; }

private:
  const envoy::extensions::filters::http::mcp::v3::Mcp::TrafficMode traffic_mode_;
  const bool clear_route_cache_;
  const absl::optional<
      envoy::extensions::filters::http::mcp::v3::Mcp::TraceContextPropagationConfig>
      propagate_trace_context_;
  const absl::optional<envoy::extensions::filters::http::mcp::v3::Mcp::BaggagePropagationConfig>
      propagate_baggage_;
  const uint32_t max_request_body_size_;
  const envoy::extensions::filters::http::mcp::v3::Mcp::RequestStorageMode request_storage_mode_;
  const std::string metadata_namespace_;
  ParserConfig parser_config_;
  McpFilterStats stats_;
};

/**
 * Per-route configuration for the MCP filter.
 */
class McpOverrideConfig : public Router::RouteSpecificFilterConfig {
public:
  explicit McpOverrideConfig(
      const envoy::extensions::filters::http::mcp::v3::McpOverride& proto_config)
      : traffic_mode_(proto_config.traffic_mode()),
        max_request_body_size_(
            proto_config.has_max_request_body_size()
                ? absl::optional<uint32_t>(proto_config.max_request_body_size().value())
                : absl::nullopt) {}

  envoy::extensions::filters::http::mcp::v3::Mcp::TrafficMode trafficMode() const {
    return traffic_mode_;
  }

  absl::optional<uint32_t> maxRequestBodySize() const { return max_request_body_size_; }

private:
  const envoy::extensions::filters::http::mcp::v3::Mcp::TrafficMode traffic_mode_;
  const absl::optional<uint32_t> max_request_body_size_;
};

using McpFilterConfigSharedPtr = std::shared_ptr<McpFilterConfig>;

/**
 * MCP proxy implementation.
 */
class McpFilter : public Http::PassThroughFilter, public Logger::Loggable<Logger::Id::mcp> {
public:
  explicit McpFilter(McpFilterConfigSharedPtr config)
      : config_(config), decoder_(decoder_config_, payload_store_) {}

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
  }

private:
  bool isValidMcpSseRequest(const Http::RequestHeaderMap& headers) const;
  bool isValidMcpPostRequest(const Http::RequestHeaderMap& headers) const;
  bool isValidMcpDeleteRequest(const Http::RequestHeaderMap& headers) const;
  bool shouldRejectRequest() const;
  uint32_t getMaxRequestBodySize() const;

  void handleParseError(absl::string_view error_msg);
  Http::FilterDataStatus completeParsing(AiProtocolManager::Codec::AiRequest& req);

  // Builds Protobuf::Struct metadata from a decoded AiRequest, extracting the
  // well-known routing fields (method, id, params.name, params.uri) that
  // mcp_filter writes to dynamic metadata and filter state.
  void buildMetadata(const AiProtocolManager::Codec::AiRequest& req, Protobuf::Struct& metadata);

  // Injects params._meta.{traceparent,tracestate,baggage} as upstream request
  // headers when the relevant config is set. Validates format before injecting.
  void injectTraceMeta(const AiProtocolManager::Codec::AgentPayload& agent);

  McpFilterConfigSharedPtr config_;
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{};
  Http::RequestHeaderMap* request_headers_{nullptr};

  // payload_store_ and decoder_config_ must be declared before decoder_ so
  // they are initialized first (C++ initializes members in declaration order).
  AiProtocolManager::Codec::InMemoryPayloadStore payload_store_;
  AiProtocolManager::Codec::DecoderConfig       decoder_config_;
  AiProtocolManager::Codec::RequestDecoder       decoder_;

  std::string raw_body_;
  size_t body_bytes_received_{0};
  bool   parsing_complete_{false};
  bool   is_mcp_request_{false};
  bool   is_json_post_request_{false};
};

} // namespace Mcp
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
