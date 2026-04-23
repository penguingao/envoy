// Functional tests for McpAuthFilter.
//
// McpAuthFilter is an AiFilter that runs inside AgenticChain (phase Q1).
// These tests exercise it directly through AiFilterChain::runRequestMetadata()
// using real Http::TestRequestHeaderMapImpl headers and AiRequest objects,
// without standing up the full Envoy HTTP stack. This is the natural boundary
// for a sub-chain filter — analogous to how McpFilterTest exercises McpFilter
// directly rather than through HttpIntegrationTest.
//
// Auth behaviors tested:
//   1. Allow-listed method ("initialize") bypasses auth entirely.
//   2. Missing identity header → 401 JSON-RPC error, no upstream.
//   3. Valid identity header → Continue + mcp.principal populated.
//   4. Admin-prefix method + non-admin principal → 403 JSON-RPC error.
//   5. Admin-prefix method + "admin" principal → Continue.
//   6. Custom allow-listed method added to config → bypasses auth.
//   7. Non-admin principal on a regular method → Continue (not an admin route).
//   8. JSON-RPC id is echoed in error body.
//   9. Notification (no id) → error body omits "id" field.

#include "source/extensions/filters/http/ai_filters/mcp_auth/config.h"
#include "source/extensions/filters/http/ai_filters/mcp_auth/filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {
namespace {

using testing::HasSubstr;

// Outcome of a single runRequestMetadata() call.
struct ChainOutcome {
  bool ready{false};        // on_ready fired (request may proceed to upstream)
  bool local_reply{false};  // on_local_reply fired (request rejected)
  int  http_status{0};
  std::string body;
};

class McpAuthFilterTest : public testing::Test {
protected:
  // Build an AiFilterChain with a single McpAuthFilter using the given config.
  // Defaults to the zero-arg McpAuthFilterConfig (identity_header="x-mcp-identity",
  // allowed_unauthenticated_methods={"initialize"}, admin_method_prefix="admin/").
  std::unique_ptr<AiProtocolManager::Chain::AiFilterChain>
  makeChain(std::shared_ptr<const McpAuthFilterConfig> cfg = nullptr) {
    if (!cfg) {
      cfg = std::make_shared<McpAuthFilterConfig>();
    }
    auto chain = std::make_unique<AiProtocolManager::Chain::AiFilterChain>();
    chain->addAiFilter(std::make_unique<McpAuthFilter>(cfg));
    chain->finalizeInterests();
    return chain;
  }

  // Build an AiRequest with the given rpc_method, optional header map pointer,
  // and optional JSON-RPC id (empty = notification).
  AiProtocolManager::Codec::AiRequest
  makeRequest(std::string method, Http::RequestHeaderMap* headers = nullptr,
              std::string jsonrpc_id = "42") {
    AiProtocolManager::Codec::AiRequest req;
    req.rpc_method  = std::move(method);
    req.jsonrpc_id  = std::move(jsonrpc_id);
    req.headers     = headers;
    req.protocol    = AiProtocolManager::Codec::ProtocolKind::AgenticMcp;
    return req;
  }

  // Run the chain and return the outcome.
  ChainOutcome run(AiProtocolManager::Chain::AiFilterChain& chain,
                   AiProtocolManager::Codec::AiRequest& req) {
    ChainOutcome out;
    chain.runRequestMetadata(
        req,
        [&out](AiProtocolManager::Codec::AiRequest&) { out.ready = true; },
        [&out](AiProtocolManager::Codec::AiResponse&& resp) {
          out.local_reply = true;
          out.http_status = resp.http_status;
          out.body        = std::move(resp.body);
        });
    return out;
  }
};

// ── 1. Allow-listed method bypasses auth ─────────────────────────────────────

TEST_F(McpAuthFilterTest, InitializeMethodBypassesAuth) {
  auto chain = makeChain();
  // No identity header — would normally cause a 401. But "initialize" is
  // always in the allow-list so auth is skipped entirely.
  auto req = makeRequest("initialize", nullptr);
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 2. Missing identity header → 401 ─────────────────────────────────────────

TEST_F(McpAuthFilterTest, MissingIdentityHeaderRejects401) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {":method", "POST"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"},
      {"content-type", "application/json"}};
  auto req = makeRequest("tools/call", &headers, "7");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("\"jsonrpc\":\"2.0\""));
  EXPECT_THAT(out.body, HasSubstr("-32001"));
  EXPECT_THAT(out.body, HasSubstr("Unauthorized"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"7\""));
}

TEST_F(McpAuthFilterTest, NullHeaderMapRejects401) {
  auto chain = makeChain();
  // headers == nullptr — no header map at all.
  auto req = makeRequest("resources/read", nullptr, "1");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32001"));
}

// ── 3. Valid identity header → Continue + principal propagated ────────────────

TEST_F(McpAuthFilterTest, ValidIdentityHeaderPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {":method", "POST"}, {"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/list", &headers, "2");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  // Principal stored for downstream filters.
  EXPECT_EQ("alice", req.attributes.at("mcp.principal"));
}

// ── 4. Admin-prefix method + non-admin principal → 403 ───────────────────────

TEST_F(McpAuthFilterTest, AdminMethodWithNonAdminPrincipalRejects403) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "bob"}};
  auto req = makeRequest("admin/restart", &headers, "3");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
  EXPECT_THAT(out.body, HasSubstr("Forbidden"));
  EXPECT_THAT(out.body, HasSubstr("admin/restart"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"3\""));
}

// ── 5. Admin-prefix method + "admin" principal → Continue ────────────────────

TEST_F(McpAuthFilterTest, AdminMethodWithAdminPrincipalPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "admin"}};
  auto req = makeRequest("admin/restart", &headers, "4");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("admin", req.attributes.at("mcp.principal"));
}

// ── 6. Custom allow-listed method bypasses auth ───────────────────────────────

TEST_F(McpAuthFilterTest, CustomAllowListedMethodBypasses) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->allowed_unauthenticated_methods.insert("ping");  // add custom entry
  auto chain = makeChain(cfg);

  // No identity header — would normally cause a 401, but "ping" is allowed.
  auto req = makeRequest("ping", nullptr, "5");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 7. Non-admin principal on a regular method → Continue ─────────────────────

TEST_F(McpAuthFilterTest, NonAdminPrincipalOnRegularMethodPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "charlie"}};
  auto req = makeRequest("resources/list", &headers, "6");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("charlie", req.attributes.at("mcp.principal"));
}

// ── 8. JSON-RPC id echoed in error body ──────────────────────────────────────

TEST_F(McpAuthFilterTest, JsonRpcIdEchoedIn401Body) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}};
  // Deliberate string id with special chars to confirm exact echo.
  auto req = makeRequest("prompts/get", &headers, "req-abc-123");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"req-abc-123\""));
}

// ── 9. Notification (empty id) → error body omits "id" field ─────────────────

TEST_F(McpAuthFilterTest, NotificationErrorBodyOmitsId) {
  auto chain = makeChain();
  // No headers → 401. Empty jsonrpc_id → "id" omitted per JSON-RPC 2.0 spec.
  auto req = makeRequest("tools/call", nullptr, "" /*notification*/);
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32001"));
  // The "id" key must not appear at all in the error body.
  EXPECT_THAT(out.body, testing::Not(HasSubstr("\"id\"")));
}

// ── 10. Custom identity header name ──────────────────────────────────────────

TEST_F(McpAuthFilterTest, CustomIdentityHeaderName) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->identity_header = "x-api-key";
  auto chain = makeChain(cfg);

  // Default "x-mcp-identity" is not present; custom "x-api-key" IS present.
  Http::TestRequestHeaderMapImpl headers{{"x-api-key", "service-account-1"}};
  auto req = makeRequest("tools/call", &headers, "8");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("service-account-1", req.attributes.at("mcp.principal"));
}

TEST_F(McpAuthFilterTest, CustomIdentityHeaderNameMissingRejects) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->identity_header = "x-api-key";
  auto chain = makeChain(cfg);

  // Old default header is present but the filter looks for "x-api-key".
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/call", &headers, "9");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
}

// ── 11. Empty admin_method_prefix disables the admin check ───────────────────

TEST_F(McpAuthFilterTest, EmptyAdminPrefixDisablesAdminCheck) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";  // disabled
  auto chain = makeChain(cfg);

  // Any principal can call "admin/op" when the prefix check is disabled.
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "regular-user"}};
  auto req = makeRequest("admin/op", &headers, "10");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
