#include "source/extensions/filters/http/mcp/mcp_filter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "envoy/buffer/buffer.h"
#include "envoy/http/codes.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/stream_info/filter_state.h"

#include "source/common/common/logger.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/common/mcp/constants.h"
#include "source/extensions/filters/common/mcp/filter_state.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "source/common/tracing/tracing_validation.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Mcp {

using FilterStateObject = Filters::Common::Mcp::FilterStateObject;

namespace {

const Http::LowerCaseString kMcpSessionId{
    std::string(Filters::Common::Mcp::McpConstants::MCP_SESSION_ID_HEADER)};

McpFilterStats generateStats(const std::string& prefix, Stats::Scope& scope) {
  const std::string final_prefix = absl::StrCat(prefix, "mcp.");
  return McpFilterStats{MCP_FILTER_STATS(POOL_COUNTER_PREFIX(scope, final_prefix))};
}

} // namespace

McpFilterConfig::McpFilterConfig(const envoy::extensions::filters::http::mcp::v3::Mcp& proto_config,
                                 const std::string& stats_prefix, Stats::Scope& scope)
    : traffic_mode_(proto_config.traffic_mode()),
      clear_route_cache_(proto_config.clear_route_cache()),
      propagate_trace_context_(proto_config.has_propagate_trace_context()
                                   ? absl::make_optional(proto_config.propagate_trace_context())
                                   : absl::nullopt),
      propagate_baggage_(proto_config.has_propagate_baggage()
                             ? absl::make_optional(proto_config.propagate_baggage())
                             : absl::nullopt),
      max_request_body_size_(proto_config.has_max_request_body_size()
                                 ? proto_config.max_request_body_size().value()
                                 : 8192), // Default: 8KB
      request_storage_mode_(proto_config.request_storage_mode()),
      metadata_namespace_(Filters::Common::Mcp::metadataNamespace()),
      parser_config_(proto_config.has_parser_config()
                         ? McpParserConfig::fromProto(proto_config.parser_config())
                         : McpParserConfig::createDefault()),
      stats_(generateStats(stats_prefix, scope)) {}

bool McpFilter::isValidMcpDeleteRequest(const Http::RequestHeaderMap& headers) const {
  // DELETE is only meaningful for MCP session termination when MCP-Session-Id is present.
  if (headers.getMethodValue() != Http::Headers::get().MethodValues.Delete) {
    return false;
  }
  return !headers.get(kMcpSessionId).empty();
}

bool McpFilter::isValidMcpSseRequest(const Http::RequestHeaderMap& headers) const {
  // Check if this is a GET request for SSE stream
  if (headers.getMethodValue() != Http::Headers::get().MethodValues.Get) {
    return false;
  }

  // Check for Accept header containing text/event-stream
  const auto& accepts = headers.get(Http::CustomHeaders::get().Accept);
  if (accepts.empty()) {
    return false;
  }

  for (size_t i = 0; i < accepts.size(); ++i) {
    if (absl::StrContains(accepts[i]->value().getStringView(),
                          Http::Headers::get().ContentTypeValues.TextEventStream)) {
      return true;
    }
  }

  return false;
}

bool McpFilter::isValidMcpPostRequest(const Http::RequestHeaderMap& headers) const {
  // Check if this is a POST request with JSON content.
  // Content-Type is JSON if it is exactly "application/json" or starts with
  // "application/json" followed by ';' or ' ' (for parameters like charset).
  // This rejects related but distinct types like application/json-patch+json.
  const absl::string_view content_type = headers.getContentTypeValue();
  const auto& json_ct = Http::Headers::get().ContentTypeValues.Json;
  bool is_json_content_type =
      absl::StartsWith(content_type, json_ct) &&
      (content_type.size() == json_ct.size() || content_type[json_ct.size()] == ';' ||
       content_type[json_ct.size()] == ' ');
  bool is_post_request =
      headers.getMethodValue() == Http::Headers::get().MethodValues.Post && is_json_content_type;

  if (!is_post_request) {
    return false;
  }

  const auto& accepts = headers.get(Http::CustomHeaders::get().Accept);
  if (accepts.empty()) {
    return false;
  }

  // Check for Accept header containing text/event-stream and application/json
  bool has_sse = false;
  bool has_json = false;

  for (size_t i = 0; i < accepts.size(); ++i) {
    const absl::string_view value = accepts[i]->value().getStringView();
    if (!has_sse &&
        absl::StrContains(value, Http::Headers::get().ContentTypeValues.TextEventStream)) {
      has_sse = true;
    }
    if (!has_json && absl::StrContains(value, Http::Headers::get().ContentTypeValues.Json)) {
      has_json = true;
    }
    if (has_sse && has_json) {
      return true;
    }
  }

  return false;
}

bool McpFilter::shouldRejectRequest() const {
  const auto* override_config =
      Http::Utility::resolveMostSpecificPerFilterConfig<McpOverrideConfig>(decoder_callbacks_);

  if (override_config) {
    return override_config->trafficMode() ==
           envoy::extensions::filters::http::mcp::v3::Mcp::REJECT_NO_MCP;
  }

  return config_->shouldRejectNonMcp();
}

uint32_t McpFilter::getMaxRequestBodySize() const {
  const auto* override_config =
      Http::Utility::resolveMostSpecificPerFilterConfig<McpOverrideConfig>(decoder_callbacks_);

  if (override_config && override_config->maxRequestBodySize().has_value()) {
    return override_config->maxRequestBodySize().value();
  }

  return config_->maxRequestBodySize();
}

Http::FilterHeadersStatus McpFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                   bool end_stream) {
  request_headers_ = &headers;

  if (isValidMcpDeleteRequest(headers)) {
    is_mcp_request_ = true;
    ENVOY_LOG(debug, "valid MCP DELETE session-termination request, passing through");
    return Http::FilterHeadersStatus::Continue;
  }

  if (isValidMcpSseRequest(headers)) {
    is_mcp_request_ = true;
    ENVOY_LOG(debug, "valid MCP SSE request, passing through");
    return Http::FilterHeadersStatus::Continue;
  }

  if (isValidMcpPostRequest(headers)) {
    is_json_post_request_ = true;
    ENVOY_LOG(debug, "valid MCP Post request");
    if (end_stream) {
      is_mcp_request_ = false;
    } else {
      // Initialize RequestDecoder for body parsing. The classifier's catch-all
      // (POST + application/json → AgenticMcp) covers all MCP paths.
      auto status = decoder_.onHeaders(headers);
      if (!status.ok()) {
        ENVOY_LOG(debug, "mcp decoder onHeaders failed: {}", status.message());
        is_mcp_request_ = false;
        is_json_post_request_ = false;
        if (shouldRejectRequest()) {
          handleParseError("MCP protocol error");
          return Http::FilterHeadersStatus::StopIteration;
        }
        return Http::FilterHeadersStatus::Continue;
      }
      is_mcp_request_ = true;

      const uint32_t max_size = getMaxRequestBodySize();
      if (max_size > 0) {
        ENVOY_LOG(debug, "set decoder buffer limit to {} bytes", max_size);
      }

      return Http::FilterHeadersStatus::StopIteration;
    }
  }

  ENVOY_LOG(debug, "after the post check");
  if (!is_mcp_request_ && shouldRejectRequest()) {
    ENVOY_LOG(debug, "rejecting non-MCP traffic");
    config_->stats().requests_rejected_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::BadRequest, "Only MCP traffic is allowed",
                                       nullptr, absl::nullopt, "mcp_filter_reject_no_mcp");
    return Http::FilterHeadersStatus::StopIteration;
  }

  ENVOY_LOG(debug, "MCP filter passing through during decoding headers");
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus McpFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  if (!is_json_post_request_ || !is_mcp_request_) {
    return Http::FilterDataStatus::Continue;
  }
  if (parsing_complete_) {
    return Http::FilterDataStatus::Continue;
  }

  ENVOY_LOG(trace, "decodeData: chunk_size={}, total_so_far={}, end_stream={}", data.length(),
            body_bytes_received_, end_stream);

  // Enforce body size limit manually since we accumulate in the decoder (not
  // Envoy's buffer), so setBufferLimit watermarks don't apply here.
  const uint32_t max_size = getMaxRequestBodySize();
  if (max_size > 0 && body_bytes_received_ + data.length() > max_size) {
    config_->stats().body_too_large_.inc();
    handleParseError("body exceeds configured size limit");
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  body_bytes_received_ += data.length();

  // Keep a raw copy for transparent passthrough: since we return
  // StopIterationNoBuffer for every non-final chunk, Envoy discards those bytes.
  // We re-inject the full body on the final chunk so the upstream sees it intact.
  raw_body_.append(data.toString());

  // Feed chunk into the decoder's internal buffer.
  auto data_status = decoder_.onData(data.toString());
  if (!data_status.ok()) {
    config_->stats().invalid_json_.inc();
    handleParseError(data_status.message());
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (!end_stream) {
    // More data coming — tell the filter manager not to buffer (we already
    // accumulated in the decoder).
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // End of stream: finalize SAX parse and extract AiRequest.
  auto end_status = decoder_.onEndStream();
  if (!end_status.ok()) {
    config_->stats().invalid_json_.inc();
    handleParseError(end_status.message());
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  auto take_result = decoder_.take();
  if (!take_result.ok()) {
    handleParseError(take_result.status().message());
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  auto status = completeParsing(take_result.value());
  if (status == Http::FilterDataStatus::Continue) {
    // Re-inject the full accumulated body so the upstream receives it intact.
    // (Previous chunks were dropped by StopIterationNoBuffer.)
    data.drain(data.length());
    data.add(raw_body_);
  }
  return status;
}

void McpFilter::handleParseError(absl::string_view error_msg) {
  ENVOY_LOG(debug, "parse error: {}", error_msg);

  is_mcp_request_ = false;

  decoder_callbacks_->sendLocalReply(Http::Code::BadRequest, error_msg, nullptr, absl::nullopt,
                                     "mcp_filter_parse_error");
}

Http::FilterDataStatus McpFilter::completeParsing(AiProtocolManager::Codec::AiRequest& req) {
  parsing_complete_ = true;

  // A valid MCP request must be JSON-RPC 2.0, classified as AgenticMcp (or
  // AgenticA2a for A2A-over-MCP), and carry a non-empty rpc_method.
  is_mcp_request_ = (req.jsonrpc_version == "2.0") &&
                    (req.protocol == AiProtocolManager::Codec::ProtocolKind::AgenticMcp ||
                     req.protocol == AiProtocolManager::Codec::ProtocolKind::AgenticA2a) &&
                    !req.rpc_method.empty();

  ENVOY_LOG(debug, "parsing complete: is_mcp={}, method={}", is_mcp_request_, req.rpc_method);

  if (!is_mcp_request_ && shouldRejectRequest()) {
    decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                       "request must be a valid JSON-RPC 2.0 message for MCP",
                                       nullptr, absl::nullopt, "mcp_filter_not_jsonrpc");
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  Protobuf::Struct metadata;
  buildMetadata(req, metadata);

  // Method group routing key.
  const std::string& group_metadata_key = config_->parserConfig().groupMetadataKey();
  if (!group_metadata_key.empty()) {
    std::string method_group = config_->parserConfig().getMethodGroup(req.rpc_method);
    (*metadata.mutable_fields())[group_metadata_key].set_string_value(method_group);
    ENVOY_LOG(debug, "MCP filter set method group: {}={}", group_metadata_key, method_group);
  }

  // Inject W3C trace context / baggage from params._meta when configured.
  if (is_mcp_request_) {
    const auto* agent = req.as_agent();
    if (agent && (!agent->meta_traceparent.empty() || !agent->meta_baggage.empty())) {
      injectTraceMeta(*agent);
    }
  }

  if (!metadata.fields().empty()) {
    if (config_->shouldStoreToFilterState()) {
      auto filter_state_obj =
          std::make_shared<FilterStateObject>(req.rpc_method, metadata, is_mcp_request_);
      decoder_callbacks_->streamInfo().filterState()->setData(
          std::string(FilterStateObject::FilterStateKey), std::move(filter_state_obj),
          StreamInfo::FilterState::StateType::ReadOnly, StreamInfo::FilterState::LifeSpan::Request,
          StreamInfo::StreamSharingMayImpactPooling::None);
    }

    if (config_->shouldStoreToDynamicMetadata()) {
      (*metadata.mutable_fields())[std::string(Filters::Common::Mcp::McpConstants::IS_MCP_REQUEST)]
          .set_bool_value(is_mcp_request_);
      decoder_callbacks_->streamInfo().setDynamicMetadata(config_->metadataNamespace(), metadata);
      ENVOY_LOG(debug, "MCP filter set dynamic metadata: {}", metadata.DebugString());
    }

    if (config_->clearRouteCache()) {
      if (auto cb = decoder_callbacks_->downstreamCallbacks(); cb.has_value()) {
        cb->clearRouteCache();
        ENVOY_LOG(debug, "MCP filter cleared route cache for metadata-based routing");
      }
    }
  }
  return Http::FilterDataStatus::Continue;
}

void McpFilter::buildMetadata(const AiProtocolManager::Codec::AiRequest& req,
                               Protobuf::Struct& metadata) {
  auto& f = *metadata.mutable_fields();

  // Envelope fields always extracted.
  if (!req.jsonrpc_version.empty()) {
    f["jsonrpc"].set_string_value(req.jsonrpc_version);
  }
  if (!req.rpc_method.empty()) {
    f["method"].set_string_value(req.rpc_method);
  }
  if (!req.jsonrpc_id.empty()) {
    f["id"].set_string_value(req.jsonrpc_id);
  }

  const auto* agent = req.as_agent();
  if (!agent) {
    return;
  }

  // Well-known routing fields extracted by AgentBodyParser. These cover the
  // most common MCP routing cases without a second JSON parse.
  if (!agent->tool_name.empty()) {
    // tools/call → params.name
    f["params.name"].set_string_value(agent->tool_name);
  } else if (!agent->resource_uri.empty()) {
    // resources/read, resources/subscribe, resources/unsubscribe → params.uri
    f["params.uri"].set_string_value(agent->resource_uri);
  } else if (!agent->prompt_name.empty()) {
    // prompts/get → params.name
    f["params.name"].set_string_value(agent->prompt_name);
  }

  // TODO: user-configured extraction rules from McpParserConfig (params.level,
  // params.ref, params.protocolVersion, etc.) require materializing params_raw
  // and applying the rules — to be implemented as a follow-up.
}

void McpFilter::injectTraceMeta(const AiProtocolManager::Codec::AgentPayload& agent) {
  if (!request_headers_) {
    return;
  }

  if (config_->propagateTraceContext().has_value()) {
    if (!agent.meta_traceparent.empty() &&
        Tracing::isValidTraceParent(agent.meta_traceparent)) {
      request_headers_->addCopy(Http::LowerCaseString("traceparent"), agent.meta_traceparent);
      ENVOY_LOG(debug, "MCP injected traceparent: {}", agent.meta_traceparent);
    }
    if (!agent.meta_tracestate.empty() &&
        Tracing::isValidTraceState(agent.meta_tracestate)) {
      request_headers_->addCopy(Http::LowerCaseString("tracestate"), agent.meta_tracestate);
      ENVOY_LOG(debug, "MCP injected tracestate: {}", agent.meta_tracestate);
    }
  }

  if (config_->propagateBaggage().has_value()) {
    if (!agent.meta_baggage.empty() && Tracing::isValidBaggage(agent.meta_baggage)) {
      request_headers_->addCopy(Http::LowerCaseString("baggage"), agent.meta_baggage);
      ENVOY_LOG(debug, "MCP injected baggage: {}", agent.meta_baggage);
    }
  }
}

} // namespace Mcp
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
