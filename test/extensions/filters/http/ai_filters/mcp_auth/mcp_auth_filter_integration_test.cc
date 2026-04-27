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
//   NonJsonRpcBodyFallsThrough         POST /mcp app/json, non-JSON-RPC body  Body parse fails → falls through

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
// arrives and fails JSON-RPC parsing (no "jsonrpc" / "method" fields), the
// filter marks the stream as non-AI and falls through rather than returning 400.
// AgenticChain (and McpAuthFilter) must NOT run on this request.

TEST_P(McpAuthFilterIntegrationTest, NonJsonRpcBodyFallsThrough) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // A plain JSON body that is NOT a JSON-RPC message — simulates a webhook or
  // other application/json endpoint sharing the same listener.
  const std::string body = R"({"type":"webhook","event":"user.created","user_id":"u_123"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  // The request must reach upstream — the filter fell through on body parse failure.
  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("webhook"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
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

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
