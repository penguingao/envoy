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
//   NonMcpGetRequestPassesThrough      GET /                                  Classified Unknown, passes without auth
//   InferenceTrafficSkipsAgenticAuth   POST /v1/chat/completions              InferenceChain (empty), passes through
//   NotificationErrorOmitsIdField      tools/list   (no id field, no header)  401, error body omits "id" per spec
//   ResourcesListWithValidIdentity…    resources/list x-mcp-identity: svc     Reaches upstream

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.h"

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
// The AiProtocolManagerFilter marks it Unknown and calls continueDecoding()
// directly — AgenticChain (and McpAuthFilter) never runs.

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

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
