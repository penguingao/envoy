// Integration tests for McpRestTranscoderIntegrationTest inside AgenticChain.
//
// End-to-end flow under test:
//
//   HTTP client
//     → AiProtocolManagerFilter (decodes JSON-RPC body, classifies as AgenticMcp)
//       → AgenticChain
//           → McpAuthFilter (Q1 auth gate — still runs when transcoder is active)
//       → AgenticDispatch
//           reads McpRestTranscoderRouteConfig from typed_per_filter_config
//           calls RequestEncoder::encodeAgentBodyAsRest()
//           mutates method/path/body to plain REST before continueDecoding()
//     → Envoy router filter
//     → upstream test server
//
// The per-route config is injected via config_helper_.addConfigModifier() onto
// the default virtual host before initialize() runs.
//
// Test matrix:
//
//   Test                                  Request                          Expected upstream
//   ──────────────────────────────────────────────────────────────────────────────────────────
//   ToolsCallTranscodedToGetQueryParam    tools/call search {"query":"hi"} GET /api/search?query=hi
//   ToolsCallTranscodedToPostWithBody     tools/call create_item {...}     POST /api/items, JSON body
//   ToolsListTranscodedToGet              tools/list                       GET /api/tools, empty body
//   ResourcesListTranscodedToGet          resources/list                   GET /api/resources, empty body
//   ResourcesReadTranscodedWithUri        resources/read uri=my-doc        GET /resources/my-doc
//   UnknownToolFallsBackToJsonRpc         tools/call unknown_tool          POST /mcp, JSON-RPC body
//   AuthFailsBeforeTranscoding            tools/call search (no identity)  401 — transcoder never runs

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

class McpRestTranscoderIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  McpRestTranscoderIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    using HCM =
        envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager;
    using AiPMProto =
        envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager;

    // Attach the REST transcoder config to the default virtual host so that
    // all routes on this listener pick it up via resolveMostSpecificPerFilterConfig.
    config_helper_.addConfigModifier([](HCM& cfg) {
      AiPMProto per_route;
      auto& tc = *per_route.mutable_rest_transcoder();

      // tools/call: "search" → GET /api/search (args become query params)
      {
        auto* t = tc.add_tools();
        t->set_tool_name("search");
        t->mutable_http_rule()->set_get("/api/search");
      }
      // tools/call: "create_item" → POST /api/items, full args as body
      {
        auto* t = tc.add_tools();
        t->set_tool_name("create_item");
        t->mutable_http_rule()->set_post("/api/items");
        t->mutable_http_rule()->set_body("*");
      }
      // tools/list  → GET /api/tools
      tc.mutable_tools_list_rule()->set_get("/api/tools");
      // resources/list → GET /api/resources
      tc.mutable_resources_list_rule()->set_get("/api/resources");
      // resources/read → GET /resources/{uri}
      tc.mutable_resources_read_rule()->set_get("/resources/{uri}");

      (*cfg.mutable_route_config()
           ->mutable_virtual_hosts()
           ->Mutable(0)
           ->mutable_typed_per_filter_config())["envoy.filters.http.ai_protocol_manager"]
          .PackFrom(per_route);
    });

    // Prepend AiProtocolManager with McpAuth inside the agentic chain.
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

INSTANTIATE_TEST_SUITE_P(IpVersions, McpRestTranscoderIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── 1. tools/call "search" → GET /api/search with args as query params ────────

TEST_P(McpRestTranscoderIntegrationTest, ToolsCallTranscodedToGetQueryParam) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"1","method":"tools/call","params":{"name":"search","arguments":{"query":"hello"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("GET"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), testing::StrEq("/api/search?query=hello"));
  EXPECT_THAT(upstream_request_->body().toString(), testing::IsEmpty());

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 2. tools/call "create_item" → POST /api/items with full args as JSON body ─

TEST_P(McpRestTranscoderIntegrationTest, ToolsCallTranscodedToPostWithBody) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"2","method":"tools/call","params":{"name":"create_item","arguments":{"name":"widget","color":"blue"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), testing::StrEq("/api/items"));
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("\"name\":\"widget\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"color\":\"blue\""));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 3. tools/list → GET /api/tools ───────────────────────────────────────────

TEST_P(McpRestTranscoderIntegrationTest, ToolsListTranscodedToGet) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = R"({"jsonrpc":"2.0","id":"3","method":"tools/list","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("GET"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), testing::StrEq("/api/tools"));
  EXPECT_THAT(upstream_request_->body().toString(), testing::IsEmpty());

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 4. resources/list → GET /api/resources ───────────────────────────────────

TEST_P(McpRestTranscoderIntegrationTest, ResourcesListTranscodedToGet) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body = R"({"jsonrpc":"2.0","id":"4","method":"resources/list","params":{}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("GET"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), testing::StrEq("/api/resources"));
  EXPECT_THAT(upstream_request_->body().toString(), testing::IsEmpty());

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 5. resources/read → GET /resources/{uri} substitution ────────────────────

TEST_P(McpRestTranscoderIntegrationTest, ResourcesReadTranscodedWithUriSubstitution) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"5","method":"resources/read","params":{"uri":"my-doc"}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("GET"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), testing::StrEq("/resources/my-doc"));
  EXPECT_THAT(upstream_request_->body().toString(), testing::IsEmpty());

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 6. Unknown tool → falls back to JSON-RPC passthrough ─────────────────────

TEST_P(McpRestTranscoderIntegrationTest, UnknownToolFallsBackToJsonRpc) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"6","method":"tools/call","params":{"name":"unknown_tool","arguments":{}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-mcp-identity", "alice"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), testing::StrEq("POST"));
  const std::string upstream_body = upstream_request_->body().toString();
  EXPECT_THAT(upstream_body, HasSubstr("tools/call"));
  EXPECT_THAT(upstream_body, HasSubstr("unknown_tool"));
  EXPECT_THAT(upstream_body, HasSubstr("jsonrpc"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 7. Auth fails before transcoding ever runs ───────────────────────────────

TEST_P(McpRestTranscoderIntegrationTest, AuthFailsBeforeTranscoding) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"jsonrpc":"2.0","id":"7","method":"tools/call","params":{"name":"search","arguments":{"query":"hello"}}})";

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
  EXPECT_THAT(response->body(), HasSubstr("Unauthorized"));
}

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
