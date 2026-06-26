#include "source/extensions/filters/http/ai_filters/mcp_auth/filter.h"

#include "source/common/json/json_streamer.h"

#include "absl/strings/match.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {

namespace {

// JSON-RPC 2.0 server-defined error codes for auth failures.
// Range: -32000 to -32099 (reserved for implementation-defined server errors).
constexpr int32_t kJsonRpcUnauthorized = -32001; // missing or unrecognised identity
constexpr int32_t kJsonRpcForbidden    = -32003; // identity present but insufficient privilege

} // namespace

McpAuthFilter::McpAuthFilter(McpAuthFilterConfigSharedPtr config) : config_(std::move(config)) {}

// ── makeLocalReply ────────────────────────────────────────────────────────────
//
// Builds a JSON-RPC 2.0 error response:
//
//   {"jsonrpc":"2.0","id":"<id>","error":{"code":<code>,"message":"<msg>"}}
//
// When jsonrpc_id is empty (notification or pre-body rejection), the "id"
// field is omitted per the JSON-RPC 2.0 specification.

AiProtocolManager::Codec::AiResponse
McpAuthFilter::makeLocalReply(int http_status, int32_t jsonrpc_error_code,
                               absl::string_view message, absl::string_view jsonrpc_id) const {
  std::string body;
  Json::StringOutput so(body);
  Json::StringStreamer streamer(so);

  auto root = streamer.makeRootMap();
  root->addKey("jsonrpc"); root->addString("2.0");
  if (!jsonrpc_id.empty()) {
    root->addKey("id"); root->addString(jsonrpc_id);
  }
  root->addKey("error");
  {
    auto err = root->addMap();
    err->addKey("code");    err->addNumber(static_cast<int64_t>(jsonrpc_error_code));
    err->addKey("message"); err->addString(message);
  }
  root.reset();

  return {http_status, std::move(body)};
}

// ── onRequestMetadata ─────────────────────────────────────────────────────────
//
// Authentication gate — runs Q1 (before any other agentic filter).
//
//  Step 1: allow-list bypass ("initialize", "ping", …).
//  Step 2: identity header lookup → principal.
//  Step 3: admin-prefix check.
//  Step 4: store principal in request.attributes for downstream filters.

AiProtocolManager::Chain::AiFilterStatus
McpAuthFilter::onRequestMetadata(AiProtocolManager::Codec::AiRequest& request,
                                  AiProtocolManager::Chain::AiFilterCallbacks& callbacks) {
  using AiProtocolManager::Chain::AiFilterStatus;

  const std::string& method = request.rpc_method;

  // ── Step 1: allow-listed methods bypass auth ─────────────────────────────
  if (config_->allowed_unauthenticated_methods.contains(method)) {
    ENVOY_LOG(debug, "mcp_auth: bypassing auth for allow-listed method '{}'", method);
    return AiFilterStatus::Continue;
  }

  // ── Step 2: identity header lookup ───────────────────────────────────────
  std::string principal;
  if (request.headers != nullptr) {
    const Http::LowerCaseString identity_key(config_->identity_header);
    const auto& entries = request.headers->get(identity_key);
    if (!entries.empty()) {
      principal = std::string(entries[0]->value().getStringView());
    }
  }

  if (principal.empty()) {
    ENVOY_LOG(debug, "mcp_auth: missing identity header '{}' for method '{}'",
              config_->identity_header, method);
    callbacks.sendLocalReply(
        makeLocalReply(401, kJsonRpcUnauthorized,
                       absl::StrCat("Unauthorized: missing '", config_->identity_header, "' header"),
                       request.jsonrpc_id));
    return AiFilterStatus::StopIteration;
  }

  // ── Step 3: access policy check ──────────────────────────────────────────
  //
  // When method_policies is non-empty, evaluate them in order; first match
  // wins. When empty, fall back to the deprecated admin_method_prefix rule.
  if (!config_->method_policies.empty()) {
    for (const auto& policy : config_->method_policies) {
      if (policy.matches(method, request)) {
        if (!policy.allows(principal)) {
          ENVOY_LOG(debug, "mcp_auth: policy denied principal '{}' for method '{}'",
                    principal, method);
          callbacks.sendLocalReply(
              makeLocalReply(403, kJsonRpcForbidden,
                             absl::StrCat("Forbidden: '", method,
                                          "' is not allowed for principal '", principal, "'"),
                             request.jsonrpc_id));
          return AiFilterStatus::StopIteration;
        }
        break; // first match wins — stop evaluating further policies
      }
    }
  } else if (!config_->admin_method_prefix.empty() &&
             absl::StartsWith(method, config_->admin_method_prefix) &&
             principal != "admin") {
    ENVOY_LOG(debug, "mcp_auth: principal '{}' is not authorised for admin method '{}'",
              principal, method);
    callbacks.sendLocalReply(
        makeLocalReply(403, kJsonRpcForbidden,
                       absl::StrCat("Forbidden: '", method, "' requires admin privileges"),
                       request.jsonrpc_id));
    return AiFilterStatus::StopIteration;
  }

  // ── Step 4: propagate principal ──────────────────────────────────────────
  // Store in request.attributes so downstream AiFilters (quota, audit, routing)
  // can read the authenticated principal without re-parsing the identity header.
  request.attributes["mcp.principal"] = principal;

  ENVOY_LOG(debug, "mcp_auth: authenticated principal '{}' for method '{}'", principal, method);
  return AiFilterStatus::Continue;
}

} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
