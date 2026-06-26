#pragma once

#include <memory>
#include <string>

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_filters/mcp_auth/config.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_callbacks.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {

// McpAuthFilter — authentication gate in the AgenticChain.
//
// Runs during Q1 (onRequestMetadata) before any other agentic filter sees the
// request. Unauthenticated or under-privileged requests are rejected via
// AiFilterCallbacks::sendLocalReply() without ever reaching the upstream.
//
// Authentication rules (from McpAuthFilterConfig):
//
//   1. allow-list bypass
//      If request.rpc_method is in allowed_unauthenticated_methods
//      (always includes "initialize"), Continue immediately — no identity
//      check is performed for the MCP handshake.
//
//   2. identity check
//      Read the value of identity_header from the HTTP request headers.
//      - Present  → principal = header value; proceed to step 3.
//      - Absent   → sendLocalReply(401) + StopIteration.
//
//   3. admin-prefix check
//      If rpc_method starts with admin_method_prefix AND principal != "admin"
//      → sendLocalReply(403) + StopIteration.
//
//   4. propagate principal
//      Store the authenticated principal in request.attributes["mcp.principal"]
//      so downstream filters (quota, audit) can read it without re-parsing
//      the identity header.
//
// The filter is read-only with respect to request payload fields: it never
// mutates AiRequest::payload, so it adds no latency from body re-encoding.
class McpAuthFilter : public AiProtocolManager::Chain::AiFilter,
                      public Logger::Loggable<Logger::Id::filter> {
public:
  explicit McpAuthFilter(McpAuthFilterConfigSharedPtr config);

  // ── AiFilter ──────────────────────────────────────────────────────────────

  AiProtocolManager::Chain::AiFilterStatus
  onRequestMetadata(AiProtocolManager::Codec::AiRequest& request,
                    AiProtocolManager::Chain::AiFilterCallbacks& callbacks) override;

  // Auth operates purely on envelope metadata — no per-item or per-chunk work.
  AiProtocolManager::Chain::AiItemKindSet itemInterest() const override {
    return AiProtocolManager::Chain::AiItemKindSet::none();
  }
  AiProtocolManager::Chain::AiChunkKindSet chunkInterest() const override {
    return AiProtocolManager::Chain::AiChunkKindSet::none();
  }

private:
  // Build the JSON-RPC error response body for the given HTTP status.
  // jsonrpc_id is echoed from the request (empty → omitted per spec).
  AiProtocolManager::Codec::AiResponse makeLocalReply(int http_status, int32_t jsonrpc_error_code,
                                                       absl::string_view message,
                                                       absl::string_view jsonrpc_id) const;

  McpAuthFilterConfigSharedPtr config_;
};

} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
