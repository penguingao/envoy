// Integration tests for McpAuthFilter inside AgenticChain.
//
// End-to-end flow under test:
//
//   HTTP client
//     → AiProtocolManagerFilter (decodes JSON-RPC body, classifies as AgenticMcp)
//       → AgenticChain
//           → McpAuthFilter (Q1 auth gate)
//       → AgenticDispatch (re-encodes body, calls continueDecoding)
//     → Envoy router filter
//     → upstream test server
//
// All requests are real JSON-RPC 2.0 POST bodies sent over HTTP/2. The
// AiProtocolManagerFilter is prepended before the router; it registers
// McpAuthFilter in its AgenticChain via buildConfig().
//
// Test matrix:
//
//   Test                               Request                                Expected
//   ─────────────────────────────────  ─────────────────────────────────────  ───────────────────────────────────────
//   InitializeBypassesAuth             initialize   (no identity header)      Reaches upstream — allow-listed
//   MissingIdentityHeaderRejects401    tools/call   (no header)               401 + -32001 JSON-RPC error
//   ValidIdentityHeaderPassesThrough   tools/call   x-mcp-identity: alice     Reaches upstream, body has myTool
//   AdminMethodNonAdminPrincipalRej…   admin/restart x-mcp-identity: bob      403 + -32003 JSON-RPC error
//   AdminMethodAdminPrincipalPasses    admin/restart x-mcp-identity: admin    Reaches upstream
//   NonMcpGetRequestPassesThrough      GET /                                  Classified NonAi, passes without auth
//   InferenceTrafficSkipsAgenticAuth   POST /v1/chat/completions              InferenceChain (empty), passes through
//   NotificationErrorOmitsIdField      tools/list   (no id field, no header)  401, error body omits "id" per spec
//   ResourcesListWithValidIdentity…    resources/list x-mcp-identity: svc     Reaches upstream
//   NonJsonRpcBodyFallsThrough         POST /mcp app/json, non-JSON-RPC body  Body parse OK, no method → falls through
//   DuplicateParamsKeyRejects400       tools/call  duplicate "params" key     400 — auth never runs
//   DuplicateMethodKeyRejects400       tools/call  duplicate "method" key     400 — auth never runs
//   DuplicateParamsAdminStillRej400    tools/call  dup "params", admin header 400 regardless of identity

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"

#include "test/integration/http_integration.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {
namespace {

using testing::HasSubstr;
using testing::Not;

class McpAuthFilterIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthFilterIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    // Prepend AiProtocolManagerFilter before the router with McpAuthFilter
    // declared in ai_filters so it is wired into the AgenticChain.
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthFilterIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── 1. Allow-listed method bypasses auth ─────────────────────────────────────
//
// "initialize" is in the default allow-list so it skips the identity check.
// The request must reach the upstream even when "x-mcp-identity" is absent.
// AgenticDispatch re-encodes the body as JSON-RPC before passing it upstream.

TEST_P(McpAuthFilterIntegrationTest, InitializeBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("initialize"));
  EXPECT_THAT(upstream_body, HasSubstr("2.0"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 2. Missing identity header → 401 ─────────────────────────────────────────
//
// A non-allow-listed method without the identity header must be rejected before
// the request ever reaches the upstream.

TEST_P(McpAuthFilterIntegrationTest, MissingIdentityHeaderRejects401) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"2","method":"tools/call","params":{"name":"myTool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  // Upstream must NOT have received the request.
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  EXPECT_THAT(response->body(), HasSubstr("Unauthorized"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"2\""));
}

// ── 3. Valid identity header → request passes to upstream ─────────────────────

TEST_P(McpAuthFilterIntegrationTest, ValidIdentityHeaderPassesThrough) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"3","method":"tools/call","params":{"name":"myTool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("tools/call"));
  EXPECT_THAT(upstream_body, HasSubstr("myTool"));
  EXPECT_THAT(upstream_body, HasSubstr("2.0"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 4. Admin method + non-admin principal → 403 ───────────────────────────────
//
// Methods starting with "admin/" require principal == "admin".
// Any other identity value in x-mcp-identity must be rejected.

TEST_P(McpAuthFilterIntegrationTest, AdminMethodNonAdminPrincipalRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // "admin/restart" is an admin-prefixed method.
  const std::string body =
      R"({"jsonrpc":"2.0","id":"4","method":"admin/restart","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("Forbidden"));
  EXPECT_THAT(response->body(), HasSubstr("admin/restart"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"4\""));
}

// ── 5. Admin method + "admin" principal → passes to upstream ──────────────────

TEST_P(McpAuthFilterIntegrationTest, AdminMethodAdminPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"5","method":"admin/restart","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("admin/restart"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 6. Non-MCP traffic passes through without auth ───────────────────────────
//
// A GET request has no JSON-RPC body and cannot be classified as AgenticMcp.
// The AiProtocolManagerFilter marks it NonAi and returns Continue directly
// without invoking AgenticChain.
TEST_P(McpAuthFilterIntegrationTest, NonMcpGetRequestPassesThrough) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{
          {":method", "GET"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"}},
      0);

  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 7. Inference traffic skips the agentic auth chain ─────────────────────────
//
// POST to /v1/chat/completions is classified as Inference, so the InferenceChain
// runs (currently empty — no auth filters). The request reaches upstream even
// without an x-mcp-identity header.

TEST_P(McpAuthFilterIntegrationTest, InferenceTrafficSkipsAgenticAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = R"({"model":"gpt-4o","messages":[{"role":"user","content":"hi"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 8. JSON-RPC notification (no "id") → error body omits id field ────────────
//
// When the JSON-RPC body has no "id" field (notification), the 401 error body
// must also omit "id" per the JSON-RPC 2.0 specification.

TEST_P(McpAuthFilterIntegrationTest, NotificationErrorOmitsIdField) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // No "id" in the JSON-RPC body → it's a notification.
  const std::string body = R"({"jsonrpc":"2.0","method":"tools/list"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  // Per JSON-RPC 2.0 spec, notifications get no "id" in the error response.
  EXPECT_THAT(response->body(), Not(HasSubstr("\"id\"")));
}

// ── 9. resources/list with valid identity → passes through ────────────────────

TEST_P(McpAuthFilterIntegrationTest, ResourcesListWithValidIdentityPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = R"({"jsonrpc":"2.0","id":"9","method":"resources/list","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "service-account-1"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("resources/list"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 10. Non-JSON-RPC body falls through to upstream ──────────────────────────
//
// The MCP classifier uses POST + application/json as a header-only heuristic,
// so any such request is tentatively classified as AgenticMcp. When the body
// arrives and parses successfully as JSON but contains no "jsonrpc"/"method"
// fields, the filter discovers rpc_method is empty during onEndStreamAndDispatch()
// and marks the stream as non-AI traffic, falling through to upstream unchanged.
// This handles genuine non-AI application/json endpoints sharing the same listener.
// Note: this is distinct from a body parse *error* (e.g. duplicate keys), which
// is rejected with 400 rather than falling through.

TEST_P(McpAuthFilterIntegrationTest, NonJsonRpcBodyFallsThrough) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // A plain JSON body that is NOT a JSON-RPC message — simulates a webhook or
  // other application/json endpoint sharing the same listener. This body is
  // valid JSON so AgentBodyParser::feed() returns OK; the fall-through happens
  // when rpc_method is found empty, not from a parse error.
  const std::string body = R"({"type":"webhook","event":"user.created","user_id":"u_123"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  // Valid JSON with no JSON-RPC fields reaches upstream unchanged.
  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("webhook"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 11. Duplicate "params" key → 400 (key-smuggling attempt rejected) ─────────
//
// A tools/call body with two "params" keys is a classic key-smuggling vector:
// the first params names a harmless tool, the second names a destructive one.
// AgentHandler::onKey() detects the duplicate via seen_params_ and the body
// parser returns InvalidArgument. The filter rejects immediately with 400 —
// auth never runs and the body never reaches upstream.

TEST_P(McpAuthFilterIntegrationTest, DuplicateParamsKeyRejects400) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm1","method":"tools/call",)"
      R"("params":{"name":"read_only_tool","arguments":{}},)"
      R"("params":{"name":"destructive_tool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("400", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("duplicate key"));
  EXPECT_THAT(response->body(), HasSubstr("params"));
}

// ── 12. Duplicate "method" key → 400 ─────────────────────────────────────────
//
// Two "method" keys allow an attacker to route through auth on an allow-listed
// method (e.g. "tools/list") while delivering a different method to the upstream
// provider. AgentHandler::onKey() detects seen_method_ and rejects with 400.

TEST_P(McpAuthFilterIntegrationTest, DuplicateMethodKeyRejects400) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm2","method":"tools/list",)"
      R"("method":"tools/call","params":{"name":"delete","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("400", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("duplicate key"));
  EXPECT_THAT(response->body(), HasSubstr("method"));
}

// ── 13. Duplicate "params" key rejected even with a valid identity header ──────
//
// Auth cannot be used to bypass the 400 path: the parse error fires before
// the AgenticChain ever runs, regardless of what identity header is present.

TEST_P(McpAuthFilterIntegrationTest, DuplicateParamsAdminIdentityStillRejects400) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm3","method":"tools/call",)"
      R"("params":{"name":"read_only_tool","arguments":{}},)"
      R"("params":{"name":"destructive_tool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("400", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("duplicate key"));
  EXPECT_THAT(response->body(), HasSubstr("params"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// method_policies integration tests
//
// Uses a separate fixture that configures McpAuthConfig with method_policies
// instead of the deprecated admin_method_prefix.
//
// Policy configured for this fixture:
//   - "admin/*"  → allowed_principals: ["admin", "ops"]
//   - "tools/*"  → allowed_principals: ["*"]    (any authenticated)
//   - "debug/*"  → allowed_principals: []        (deny all)
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthPolicyIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthPolicyIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            method_policies:
            - method_pattern: "admin/*"
              allowed_principals: ["admin", "ops"]
            - method_pattern: "tools/*"
              allowed_principals: ["*"]
            - method_pattern: "debug/*"
              allowed_principals: []
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthPolicyIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── P1. Permitted principal for admin/* passes ───────────────────────────────
//
// "ops" is listed in the admin/* policy → request reaches upstream.

TEST_P(McpAuthPolicyIntegrationTest, PolicyAdminMethodPermittedPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p1","method":"admin/restart","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "ops"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("admin/restart"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── P2. Non-permitted principal for admin/* → 403 ────────────────────────────
//
// "bob" is not in the admin/* allowed_principals list → 403 JSON-RPC error.

TEST_P(McpAuthPolicyIntegrationTest, PolicyAdminMethodNonPermittedPrincipalRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p2","method":"admin/restart","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("Forbidden"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"p2\""));
}

// ── P3. Wildcard principal for tools/* passes any authenticated identity ──────
//
// tools/* has allowed_principals: ["*"] — any identity header value passes.

TEST_P(McpAuthPolicyIntegrationTest, PolicyWildcardPrincipalAllowsAnyIdentity) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p3","method":"tools/call","params":{"name":"myTool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "anyone-at-all"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("tools/call"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── P4. Empty allowed_principals for debug/* denies all authenticated ─────────
//
// debug/* has allowed_principals: [] — even a valid identity header is denied.

TEST_P(McpAuthPolicyIntegrationTest, PolicyEmptyPrincipalsDenyAllAuthenticated) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p4","method":"debug/heap","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"p4\""));
}

// ── P5. No matching policy → default allow for authenticated principal ────────
//
// "resources/list" is not covered by any rule → any authenticated principal
// passes through.

TEST_P(McpAuthPolicyIntegrationTest, PolicyNoMatchDefaultAllow) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p5","method":"resources/list","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("resources/list"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── P6. method_policies: missing identity header still rejected 401 ───────────
//
// The identity check (step 2) runs before any policy evaluation. Even with
// method_policies configured, a missing header is still a 401.

TEST_P(McpAuthPolicyIntegrationTest, PolicyMissingIdentityHeaderStillRejects401) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p6","method":"tools/call","params":{"name":"myTool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"p6\""));
}

// ── P7. method_policies: initialize still bypasses auth via allow-list ────────
//
// The allow-list check (step 1) runs before policy evaluation, so "initialize"
// reaches upstream even with no identity header and restrictive policies set.

TEST_P(McpAuthPolicyIntegrationTest, PolicyInitializeStillBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"p7","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("initialize"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ═══════════════════════════════════════════════════════════════════════════════
// param_conditions integration tests
//
// A separate fixture that configures method_policies with param_conditions:
//
//   - tools/call  + tool_name exact "search"     → allowed_principals: ["*"]
//   - tools/call  + tool_name exact "delete"     → allowed_principals: ["admin"]
//   - tools/call  (no param condition, catch-all) → allowed_principals: ["*"]
//   - resources/read + resource_uri prefix "public/" → allowed_principals: ["*"]
//   - resources/read (catch-all)                 → allowed_principals: ["alice"]
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthParamConditionIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthParamConditionIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            method_policies:
            - method_pattern: "tools/call"
              allowed_principals: ["*"]
              param_conditions:
              - field: TOOL_NAME
                matcher:
                  exact: "search"
            - method_pattern: "tools/call"
              allowed_principals: ["admin"]
              param_conditions:
              - field: TOOL_NAME
                matcher:
                  exact: "delete"
            - method_pattern: "tools/call"
              allowed_principals: ["*"]
            - method_pattern: "resources/read"
              allowed_principals: ["*"]
              param_conditions:
              - field: RESOURCE_URI
                matcher:
                  prefix: "public/"
            - method_pattern: "resources/read"
              allowed_principals: ["alice"]
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthParamConditionIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── PC1. tools/call search — any authenticated principal passes ───────────────
//
// tool_name "search" matches the first rule (allowed_principals: ["*"]).

TEST_P(McpAuthParamConditionIntegrationTest, ToolCallSearchAnyPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc1","method":"tools/call","params":{"name":"search","arguments":{"query":"hi"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("search"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PC2. tools/call delete — non-admin principal → 403 ───────────────────────
//
// tool_name "delete" matches the second rule (allowed_principals: ["admin"]).
// "bob" is not admin → 403.

TEST_P(McpAuthParamConditionIntegrationTest, ToolCallDeleteNonAdminRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc2","method":"tools/call","params":{"name":"delete","arguments":{"id":"123"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"pc2\""));
}

// ── PC3. tools/call delete — admin principal passes ──────────────────────────

TEST_P(McpAuthParamConditionIntegrationTest, ToolCallDeleteAdminPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc3","method":"tools/call","params":{"name":"delete","arguments":{"id":"123"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("delete"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PC4. tools/call unknown tool — falls to catch-all rule → any principal ───
//
// tool_name "rename" matches neither the "search" nor "delete" param rules,
// so evaluation falls to the third rule (tools/call, no param condition) which
// allows everyone.

TEST_P(McpAuthParamConditionIntegrationTest, ToolCallUnknownToolCatchAllPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc4","method":"tools/call","params":{"name":"rename","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "anyone"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("rename"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PC5. resources/read public/ URI — any principal passes ───────────────────

TEST_P(McpAuthParamConditionIntegrationTest, ResourceReadPublicUriAnyPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc5","method":"resources/read","params":{"uri":"public/readme"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("public/readme"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PC6. resources/read private/ URI — falls to catch-all → only alice ───────
//
// URI "private/secrets" doesn't match the "public/" prefix condition, so the
// first resources/read rule is skipped. The second rule (no param condition)
// allows only "alice". "bob" → 403, "alice" → passes.

TEST_P(McpAuthParamConditionIntegrationTest, ResourceReadPrivateUriNonAliceRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc6","method":"resources/read","params":{"uri":"private/secrets"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
}

TEST_P(McpAuthParamConditionIntegrationTest, ResourceReadPrivateUriAlicePasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pc7","method":"resources/read","params":{"uri":"private/secrets"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("private/secrets"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Large-payload tests — external buffer storage path
//
// These tests send JSON-RPC bodies whose total size exceeds the default inline
// threshold (4096 bytes), forcing the MmapPayloadStore to write the params and
// residual_params to its backing mmap region (Storage::External) rather than
// keeping them on the heap.
//
// Three properties are verified end-to-end:
//   1. The full body is correctly parsed (tool_name / method extracted)
//   2. Auth is still enforced regardless of body size
//   3. The upstream receives the complete, untruncated payload
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: builds a JSON-RPC tools/call body whose arguments.data field is
// padded to ensure the total serialised size exceeds the 4096-byte mmap
// threshold.  The body is valid JSON and round-trips through AgentBodyParser.
std::string makeLargeToolsCallBody(const std::string& id, const std::string& tool_name,
                                   size_t pad_bytes = 4500) {
  return R"({"jsonrpc":"2.0","id":")" + id +
         R"(","method":"tools/call","params":{"name":")" + tool_name +
         R"(","arguments":{"data":")" + std::string(pad_bytes, 'A') + R"("}}})";
}

// ── L1. Large tools/call body + valid identity → reaches upstream intact ──────
//
// With MmapPayloadStore the arguments (>4096 bytes) are stored as
// Storage::External and fetched back by AgentDispatch before re-encoding.
// The upstream must receive a body containing the method and tool name.

TEST_P(McpAuthFilterIntegrationTest, LargeToolsCallBodyWithValidAuthPassesThrough) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = makeLargeToolsCallBody("L1", "myTool");
  ASSERT_GT(body.size(), 4096u) << "test precondition: body must exceed inline threshold";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_GT(upstream_body.size(), 4096u) << "upstream body should not be truncated";
  EXPECT_THAT(upstream_body, HasSubstr("tools/call"));
  EXPECT_THAT(upstream_body, HasSubstr("myTool"));
  // Spot-check: a run of 'A' chars from the large payload is present.
  EXPECT_THAT(upstream_body, HasSubstr(std::string(100, 'A')));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── L2. Large tools/call body + missing identity → 401 ────────────────────────
//
// Auth is evaluated after body parsing: even for a >4096-byte body that goes
// through External storage, a missing identity header must still yield 401.

TEST_P(McpAuthFilterIntegrationTest, LargeToolsCallBodyWithoutAuthRejects401) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = makeLargeToolsCallBody("L2", "myTool");

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"L2\""));
}

// ── L3. Large initialize body (allow-listed) passes without identity header ───
//
// "initialize" is in the allow-list regardless of body size.  The large params
// (>4096 bytes) are stored as External refs; the request still reaches upstream.

TEST_P(McpAuthFilterIntegrationTest, LargeInitializeBodyBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // Build a large initialize body by padding the capabilities object.
  const std::string large_cap(4500, 'C');
  const std::string body =
      R"({"jsonrpc":"2.0","id":"L3","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{"extra":")" +
      large_cap + R"("}}})" ;
  ASSERT_GT(body.size(), 4096u);

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_GT(upstream_body.size(), 4096u);
  EXPECT_THAT(upstream_body, HasSubstr("initialize"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── LP1. Policy: large tools/call body + wildcard policy passes ───────────────
//
// Exercises the External storage path through McpAuthPolicyIntegrationTest
// to confirm policy evaluation is unaffected by the storage backend.

TEST_P(McpAuthPolicyIntegrationTest, PolicyLargeToolsCallWildcardPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = makeLargeToolsCallBody("LP1", "rename");
  ASSERT_GT(body.size(), 4096u);

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "anyone"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_GT(upstream_body.size(), 4096u);
  EXPECT_THAT(upstream_body, HasSubstr("tools/call"));
  EXPECT_THAT(upstream_body, HasSubstr("rename"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PC-L1. Param condition: large body, tool_name still extracted correctly ───
//
// Confirms that AgentBodyParser correctly extracts tool_name from a large body
// (stored in External MmapPayloadStore) so the param_condition can match it.

TEST_P(McpAuthParamConditionIntegrationTest, LargeBodyToolNameExtractedForParamCondition) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // "search" with large extra field — should match the wildcard tools/* rule.
  const std::string body = makeLargeToolsCallBody("PCL1", "search");
  ASSERT_GT(body.size(), 4096u);

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_GT(upstream_body.size(), 4096u);
  EXPECT_THAT(upstream_body, HasSubstr("search"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ═══════════════════════════════════════════════════════════════════════════════
// allowed_unauthenticated_methods tests
//
// Verifies that additional methods added to the allow-list bypass the identity
// check, while non-listed methods still require authentication, and that
// "initialize" is always present even without explicit configuration.
//
// Config: allowed_unauthenticated_methods = ["ping", "notifications/initialized"]
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthAllowListIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthAllowListIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            allowed_unauthenticated_methods: ["ping", "notifications/initialized"]
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthAllowListIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── AL1. Configured allow-listed method bypasses auth ────────────────────────

TEST_P(McpAuthAllowListIntegrationTest, PingBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = R"({"jsonrpc":"2.0","id":"al1","method":"ping","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("ping"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── AL2. Second configured allow-listed method also bypasses ─────────────────

TEST_P(McpAuthAllowListIntegrationTest, NotificationsInitializedBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"al2","method":"notifications/initialized","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("notifications/initialized"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── AL3. "initialize" is always in the allow-list regardless of config ────────
//
// McpAuthFilterConfig::McpAuthFilterConfig() always inserts "initialize".
// Even without it in the proto's allowed_unauthenticated_methods, it passes.

TEST_P(McpAuthAllowListIntegrationTest, InitializeAlwaysBypassesAuth) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"al3","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("initialize"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── AL4. Non-allow-listed method still requires the identity header ────────────

TEST_P(McpAuthAllowListIntegrationTest, NonAllowListedMethodRequiresIdentity) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"al4","method":"tools/call","params":{"name":"myTool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"al4\""));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Suffix matcher + multiple param conditions (AND semantics) tests
//
// Exercises the suffix variant of StringMatcher and the AND semantics when
// multiple param_conditions appear on the same policy rule.
//
// Policy configured for this fixture:
//   - resources/read  + resource_uri prefix "secure/" AND suffix ".json"
//                     → allowed_principals: ["*"]
//   - resources/read  (catch-all)
//                     → allowed_principals: ["admin"]
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthSuffixAndMultiConditionIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthSuffixAndMultiConditionIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            method_policies:
            - method_pattern: "resources/read"
              allowed_principals: ["*"]
              param_conditions:
              - field: RESOURCE_URI
                matcher:
                  prefix: "secure/"
              - field: RESOURCE_URI
                matcher:
                  suffix: ".json"
            - method_pattern: "resources/read"
              allowed_principals: ["admin"]
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthSuffixAndMultiConditionIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── SM1. URI satisfying both conditions → any authenticated principal passes ──
//
// "secure/config.json" has the "secure/" prefix AND the ".json" suffix.
// Both conditions hold → rule 1 matches → any authenticated principal passes.

TEST_P(McpAuthSuffixAndMultiConditionIntegrationTest, BothConditionsMatchAnyPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm1","method":"resources/read","params":{"uri":"secure/config.json"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("secure/config.json"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── SM2. Prefix matches but suffix fails → rule 1 skipped, catch-all → 403 ───
//
// "secure/config.xml" has the "secure/" prefix but NOT the ".json" suffix.
// Rule 1 does not match (AND requires both) → falls to catch-all →
// only "admin" allowed → "bob" gets 403.

TEST_P(McpAuthSuffixAndMultiConditionIntegrationTest, SuffixMismatchFallsToCatchAll403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm2","method":"resources/read","params":{"uri":"secure/config.xml"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"sm2\""));
}

// ── SM3. Suffix matches but prefix fails → rule 1 skipped, catch-all → 403 ───
//
// "public/config.json" ends in ".json" but does NOT start with "secure/".
// Rule 1 does not match → falls to catch-all → non-admin "bob" gets 403.

TEST_P(McpAuthSuffixAndMultiConditionIntegrationTest, PrefixMismatchFallsToCatchAll403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm3","method":"resources/read","params":{"uri":"public/config.json"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "bob"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"sm3\""));
}

// ── SM4. Admin principal always passes via the catch-all rule ─────────────────

TEST_P(McpAuthSuffixAndMultiConditionIntegrationTest, CatchAllAdminPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"sm4","method":"resources/read","params":{"uri":"secure/config.xml"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("resources/read"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ═══════════════════════════════════════════════════════════════════════════════
// prompts/get + PROMPT_NAME param condition tests
//
// Verifies end-to-end that AgentBodyParser extracts prompt_name from
// params.name for prompts/get requests and that McpAuthFilter evaluates
// PROMPT_NAME param conditions correctly.
//
// Policy configured for this fixture:
//   - prompts/get + prompt_name exact "system-prompt" → allowed_principals: ["admin"]
//   - prompts/get (catch-all)                         → allowed_principals: ["*"]
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthPromptNameIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthPromptNameIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            method_policies:
            - method_pattern: "prompts/get"
              allowed_principals: ["admin"]
              param_conditions:
              - field: PROMPT_NAME
                matcher:
                  exact: "system-prompt"
            - method_pattern: "prompts/get"
              allowed_principals: ["*"]
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthPromptNameIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── PN1. Restricted prompt + non-admin principal → 403 ───────────────────────
//
// "system-prompt" matches the first rule (admin only). "alice" is not admin.

TEST_P(McpAuthPromptNameIntegrationTest, RestrictedPromptNonAdminRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pn1","method":"prompts/get","params":{"name":"system-prompt"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"pn1\""));
}

// ── PN2. Restricted prompt + admin principal → passes ────────────────────────

TEST_P(McpAuthPromptNameIntegrationTest, RestrictedPromptAdminPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pn2","method":"prompts/get","params":{"name":"system-prompt"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "admin"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("prompts/get"));
  EXPECT_THAT(upstream_body, HasSubstr("system-prompt"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PN3. Non-restricted prompt → any authenticated principal passes ────────────
//
// "user-prompt" does not match the "system-prompt" exact condition, so rule 1
// is skipped. The catch-all allows any authenticated principal.

TEST_P(McpAuthPromptNameIntegrationTest, UnrestrictedPromptAnyPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pn3","method":"prompts/get","params":{"name":"user-prompt"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("prompts/get"));
  EXPECT_THAT(upstream_body, HasSubstr("user-prompt"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── PN4. prompts/get with no identity header → 401 ───────────────────────────
//
// "prompts/get" is not in the default allow-list. A missing identity header
// is still rejected at step 2 before any policy is evaluated.

TEST_P(McpAuthPromptNameIntegrationTest, PromptGetMissingIdentityRejects401) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"pn4","method":"prompts/get","params":{"name":"user-prompt"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("401", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32001"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"pn4\""));
}

// ═══════════════════════════════════════════════════════════════════════════════
// ATTRIBUTE param_condition integration tests (depth >= 3 extraction)
//
// Exercises the full pipeline: the Wuffs decoder extracts a depth-3 field
// (params.arguments.database) via DecoderConfig.extract_fields and stores it
// in AiRequest.attributes; McpAuthFilter evaluates a ParamCondition with
// field: ATTRIBUTE to gate access based on that extracted value.
//
// Policy configured for this fixture:
//   - tools/call + ATTRIBUTE "params.arguments.database" exact "prod"
//                → allowed_principals: ["dba"]
//   - tools/call + ATTRIBUTE "params.arguments.database" exact "staging"
//                → allowed_principals: ["*"]
//   - tools/call  (catch-all)
//                → allowed_principals: ["*"]
// ═══════════════════════════════════════════════════════════════════════════════

class McpAuthAttributeParamConditionIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpAuthAttributeParamConditionIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
        decoder_config:
          extract_fields:
          - json_path: "params.arguments.database"
        ai_filters:
        - name: envoy.ai_filters.mcp_auth
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
            method_policies:
            - method_pattern: "tools/call"
              allowed_principals: ["dba"]
              param_conditions:
              - field: ATTRIBUTE
                attribute_key: "params.arguments.database"
                matcher:
                  exact: "prod"
            - method_pattern: "tools/call"
              allowed_principals: ["*"]
              param_conditions:
              - field: ATTRIBUTE
                attribute_key: "params.arguments.database"
                matcher:
                  exact: "staging"
            - method_pattern: "tools/call"
              allowed_principals: ["*"]
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpAuthAttributeParamConditionIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── AT1. Depth-3 attribute "prod" — only dba principal allowed ────────────────
//
// The Wuffs decoder extracts params.arguments.database = "prod" from the body.
// The first policy matches; "alice" is not "dba" → 403.

TEST_P(McpAuthAttributeParamConditionIntegrationTest, ProdDatabaseNonDbaRejects403) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"at1","method":"tools/call","params":{"name":"query","arguments":{"database":"prod","sql":"SELECT 1"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_FALSE(upstream_request_ != nullptr);
  EXPECT_EQ("403", response->headers().getStatusValue());
  EXPECT_THAT(response->body(), HasSubstr("-32003"));
  EXPECT_THAT(response->body(), HasSubstr("\"id\":\"at1\""));
}

// ── AT2. Depth-3 attribute "prod" — dba principal passes ─────────────────────
//
// Same body, identity "dba" → matches the first policy → allowed.

TEST_P(McpAuthAttributeParamConditionIntegrationTest, ProdDatabaseDbapasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"at2","method":"tools/call","params":{"name":"query","arguments":{"database":"prod","sql":"SELECT 1"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "dba"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("query"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── AT3. Depth-3 attribute "staging" — any authenticated principal passes ─────
//
// database = "staging" skips the first policy (condition fails) and matches the
// second policy (allowed_principals: ["*"]).

TEST_P(McpAuthAttributeParamConditionIntegrationTest, StagingDatabaseAnyPrincipalPasses) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"at3","method":"tools/call","params":{"name":"query","arguments":{"database":"staging","sql":"SELECT 1"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("query"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── AT4. Attribute absent (no database key) — falls to catch-all → any principal
//
// When params.arguments.database is not present in the body, the attribute is
// not extracted, so both ATTRIBUTE conditions fail. The catch-all rule (no
// param conditions) matches and any authenticated principal passes.

TEST_P(McpAuthAttributeParamConditionIntegrationTest, MissingAttributeFallsToCatchAll) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // No "database" key in arguments.
  const std::string body =
      R"({"jsonrpc":"2.0","id":"at4","method":"tools/call","params":{"name":"ping","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("ping"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
