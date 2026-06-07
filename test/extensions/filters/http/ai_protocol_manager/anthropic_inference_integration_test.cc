// Integration tests for the Anthropic inference translation path.
//
// End-to-end flow under test:
//
//   HTTP client (OpenAI format)
//     → AiProtocolManagerFilter
//         decodeHeaders: x-ai-provider: anthropic → sets provider_hint
//         decodeData:    body buffered by InferenceBodyParser
//         onEndStream:   InferenceBodyParser::finish populates InferencePayload
//       → InferenceChain (empty — no AI chain filters in this suite)
//       → InferenceDispatch
//           provider_hint == "anthropic" → AnthropicRequestEncoder::encode()
//           headers mutated: :path → /v1/messages
//           body injected:   Anthropic Messages API JSON
//     → Envoy router filter
//     → upstream test server (receives Anthropic-format request)
//
// Without x-ai-provider the filter round-trips the OpenAI body unchanged.
//
// Test matrix:
//
//   Test                                     Sends                              Expects at upstream
//   ───────────────────────────────────────  ─────────────────────────────────  ──────────────────────────────────
//   BasicChatCompletionToAnthropicFormat     OpenAI chat + x-ai-provider        path=/v1/messages, system extracted
//   SystemMessageExtractedFromMessages       messages w/ system + user           "system" top-level, no role:system
//   ChatCompletionWithoutProviderRoundTrips  OpenAI chat, no header              path=/v1/chat/completions, unchanged
//   ToolDefinitionsTranslated                tools[].function.parameters        input_schema, no nested "function"
//   AssistantToolCallsConverted              assistant message w/ tool_calls     tool_use content blocks
//   ConsecutiveToolResultsMerged             two role:tool messages              single user turn, two tool_result blocks
//   ToolChoiceRequiredToAny                  tool_choice:"required"              {"type":"any"}
//   ToolChoiceAutoToObject                   tool_choice:"auto"                  {"type":"auto"}
//   ToolChoiceNamedFunctionConverted         tool_choice:{type:function,name}    {"type":"tool","name":"..."}
//   MaxTokensDefaultApplied                  body without max_tokens             max_tokens:4096
//   MaxTokensPreservedIfSet                  max_tokens:512                      max_tokens:512
//   StopRenamedToStopSequences               stop:["\n"]                         stop_sequences, no bare "stop"
//   SamplingParamsPreserved                  temperature, top_p                  same values in Anthropic body
//   StreamFlagPreserved                      stream:true                         stream:true
//   NAndSeedDropped                          n:3, seed:42                        neither field in Anthropic body
//   LegacyCompletionWrapsPromptAsUserMsg     POST /v1/completions + prompt       messages:[{role:user,content:prompt}]
//   MultipleSystemMessagesJoined             two system messages                 single "system" value
//   UnknownFieldsForwardedVerbatimInRoundTrip  response_format + stream_options + user  all three present at upstream

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"

#include "test/integration/http_integration.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using testing::HasSubstr;
using testing::Not;

// ── Fixture ───────────────────────────────────────────────────────────────────
//
// Registers AiProtocolManagerFilter with no AI chain filters — we are testing
// the translation logic in InferenceDispatch / AnthropicRequestEncoder, not
// any chain filter behaviour.

class AnthropicInferenceIntegrationTest
    : public testing::TestWithParam<Network::Address::IpVersion>,
      public HttpIntegrationTest {
public:
  AnthropicInferenceIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void initialize() override {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
    )EOF");
    HttpIntegrationTest::initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, AnthropicInferenceIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// ── 1. Basic translation: path rewritten, system extracted ────────────────────
//
// With x-ai-provider: anthropic the filter must:
//   - Rewrite :path from /v1/chat/completions to /v1/messages
//   - Extract the system message to a top-level "system" string
//   - Remove the system turn from the messages array
//   - Preserve the user message

TEST_P(AnthropicInferenceIntegrationTest, BasicChatCompletionToAnthropicFormat) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","messages":[)"
      R"({"role":"system","content":"You are a helpful assistant."},)"
      R"({"role":"user","content":"Hello"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_path =
      std::string(upstream_request_->headers().getPathValue());
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_EQ("/v1/messages", upstream_path);
  EXPECT_EQ("POST", std::string(upstream_request_->headers().getMethodValue()));
  EXPECT_THAT(upstream_body, HasSubstr("\"system\":\"You are a helpful assistant.\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"user\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"content\":\"Hello\""));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"role\":\"system\"")));
  EXPECT_THAT(upstream_body, HasSubstr("\"model\":\"claude-3-5-sonnet-20241022\""));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 2. System message extracted; non-system turns preserved ───────────────────

TEST_P(AnthropicInferenceIntegrationTest, SystemMessageExtractedFromMessages) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-opus-4-5","messages":[)"
      R"({"role":"system","content":"Be concise."},)"
      R"({"role":"user","content":"What is 2+2?"},)"
      R"({"role":"assistant","content":"4."},)"
      R"({"role":"user","content":"Why?"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"system\":\"Be concise.\""));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"role\":\"system\"")));
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"user\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"assistant\""));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 3. No x-ai-provider → OpenAI round-trip, path unchanged ──────────────────
//
// Without x-ai-provider the filter must pass the body through unchanged
// (OpenAI round-trip). The path must NOT be rewritten to /v1/messages and the
// system message must remain inside messages[].

TEST_P(AnthropicInferenceIntegrationTest, ChatCompletionWithoutProviderRoundTrips) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"gpt-4o","messages":[)"
      R"({"role":"system","content":"You are helpful."},)"
      R"({"role":"user","content":"Hi"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_path =
      std::string(upstream_request_->headers().getPathValue());
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_EQ("/v1/chat/completions", upstream_path);
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"system\""));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"system\":\"You are helpful.\"")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 4. Tool definitions translated: parameters → input_schema ─────────────────
//
// OpenAI wraps tool definitions under "function"; Anthropic uses a flat
// structure with "input_schema" instead of "parameters".

TEST_P(AnthropicInferenceIntegrationTest, ToolDefinitionsTranslated) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Search"}],)"
      R"("tools":[{"type":"function","function":{"name":"web_search",)"
      R"("description":"Search the web","parameters":{"type":"object",)"
      R"("properties":{"query":{"type":"string"}}}}}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"name\":\"web_search\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"description\":\"Search the web\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"input_schema\""));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"function\":")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 5. Assistant tool_calls converted to tool_use content blocks ──────────────
//
// OpenAI assistant messages carry tool invocations under "tool_calls[]".
// Anthropic expects these as "tool_use" content blocks inside "content[]".
// The "arguments" JSON string must be parsed into an object for "input".

TEST_P(AnthropicInferenceIntegrationTest, AssistantToolCallsConverted) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","messages":[)"
      R"({"role":"user","content":"Get weather"},)"
      R"({"role":"assistant","content":null,"tool_calls":[)"
      R"({"id":"call_abc","type":"function","function":)"
      R"({"name":"get_weather","arguments":"{\"city\":\"NYC\"}"}}]}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"type\":\"tool_use\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"id\":\"call_abc\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"name\":\"get_weather\""));
  // "input" must be a parsed object, not a raw string.
  EXPECT_THAT(upstream_body, HasSubstr("\"input\":{\"city\":\"NYC\"}"));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"tool_calls\":")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 6. Consecutive role:tool messages merged into one user turn ───────────────
//
// Anthropic requires all tool results in a single user turn as tool_result
// content blocks. Consecutive OpenAI "tool" role messages must be merged.

TEST_P(AnthropicInferenceIntegrationTest, ConsecutiveToolResultsMerged) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","messages":[)"
      R"({"role":"user","content":"Run both tools"},)"
      R"({"role":"assistant","content":null,"tool_calls":[)"
      R"({"id":"call_1","type":"function","function":{"name":"tool_a","arguments":"{}"}},)"
      R"({"id":"call_2","type":"function","function":{"name":"tool_b","arguments":"{}"}}]},)"
      R"({"role":"tool","tool_call_id":"call_1","content":"result_a"},)"
      R"({"role":"tool","tool_call_id":"call_2","content":"result_b"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"type\":\"tool_result\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"tool_use_id\":\"call_1\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"tool_use_id\":\"call_2\""));
  EXPECT_THAT(upstream_body, HasSubstr("result_a"));
  EXPECT_THAT(upstream_body, HasSubstr("result_b"));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"role\":\"tool\"")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 7. tool_choice "required" → {"type":"any"} ────────────────────────────────

TEST_P(AnthropicInferenceIntegrationTest, ToolChoiceRequiredToAny) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Use a tool"}],)"
      R"("tools":[{"type":"function","function":{"name":"f","parameters":{"type":"object"}}}],)"
      R"("tool_choice":"required"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(),
              HasSubstr("\"tool_choice\":{\"type\":\"any\"}"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 8. tool_choice "auto" → {"type":"auto"} ───────────────────────────────────

TEST_P(AnthropicInferenceIntegrationTest, ToolChoiceAutoToObject) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Go"}],)"
      R"("tools":[{"type":"function","function":{"name":"f","parameters":{"type":"object"}}}],)"
      R"("tool_choice":"auto"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(),
              HasSubstr("\"tool_choice\":{\"type\":\"auto\"}"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 9. tool_choice named function → {"type":"tool","name":"..."} ──────────────

TEST_P(AnthropicInferenceIntegrationTest, ToolChoiceNamedFunctionConverted) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Go"}],)"
      R"("tools":[{"type":"function","function":{"name":"calc","parameters":{"type":"object"}}}],)"
      R"("tool_choice":{"type":"function","function":{"name":"calc"}}})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"type\":\"tool\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"name\":\"calc\""));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"type\":\"function\"")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 10. max_tokens defaults to 4096 when absent ───────────────────────────────
//
// Anthropic requires max_tokens; inject a default when the OpenAI caller
// omitted it so the upstream does not reject the request.

TEST_P(AnthropicInferenceIntegrationTest, MaxTokensDefaultApplied) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","messages":[{"role":"user","content":"Hi"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("\"max_tokens\":4096"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 11. Explicit max_tokens is forwarded as-is ────────────────────────────────

TEST_P(AnthropicInferenceIntegrationTest, MaxTokensPreservedIfSet) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","max_tokens":512,)"
      R"("messages":[{"role":"user","content":"Hi"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"max_tokens\":512"));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"max_tokens\":4096")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 12. stop renamed to stop_sequences ────────────────────────────────────────
//
// Anthropic uses "stop_sequences" (array) instead of OpenAI's "stop".
// The field must be renamed and the bare "stop" key must not appear.

TEST_P(AnthropicInferenceIntegrationTest, StopRenamedToStopSequences) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Hi"}],)"
      R"("stop":["\n\n","Human:"]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"stop_sequences\""));
  EXPECT_THAT(upstream_body, HasSubstr("Human:"));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"stop\":")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 13. Sampling parameters preserved ────────────────────────────────────────

TEST_P(AnthropicInferenceIntegrationTest, SamplingParamsPreserved) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Hi"}],)"
      R"("temperature":0.7,"top_p":0.9})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("\"temperature\":0.7"));
  EXPECT_THAT(upstream_body, HasSubstr("\"top_p\":0.9"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 14. stream flag preserved ─────────────────────────────────────────────────

TEST_P(AnthropicInferenceIntegrationTest, StreamFlagPreserved) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Hi"}],)"
      R"("stream":true})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->body().toString(), HasSubstr("\"stream\":true"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 15. n and seed dropped ────────────────────────────────────────────────────
//
// Anthropic's Messages API does not support "n" or "seed". They must be
// silently dropped and must not appear in the upstream body.

TEST_P(AnthropicInferenceIntegrationTest, NAndSeedDropped) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022",)"
      R"("messages":[{"role":"user","content":"Hi"}],)"
      R"("n":3,"seed":42})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, Not(HasSubstr("\"n\":")));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"seed\":")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 16. Legacy /v1/completions: prompt wrapped as user message ────────────────
//
// The older Completions API uses a "prompt" string. AnthropicRequestEncoder
// wraps it as a user message so the upstream Anthropic Messages API can handle
// it without any changes to the calling application.

TEST_P(AnthropicInferenceIntegrationTest, LegacyCompletionWrapsPromptAsUserMsg) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","prompt":"Tell me a joke","max_tokens":100})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_path =
      std::string(upstream_request_->headers().getPathValue());
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_EQ("/v1/messages", upstream_path);
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"user\""));
  EXPECT_THAT(upstream_body, HasSubstr("Tell me a joke"));
  EXPECT_THAT(upstream_body, HasSubstr("\"max_tokens\":100"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 17. Multiple system messages are concatenated ─────────────────────────────
//
// When the OpenAI messages array contains more than one system turn they are
// joined with "\n\n" into a single Anthropic "system" string.

TEST_P(AnthropicInferenceIntegrationTest, MultipleSystemMessagesJoined) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"claude-3-5-sonnet-20241022","messages":[)"
      R"({"role":"system","content":"You are concise."},)"
      R"({"role":"system","content":"Reply in English."},)"
      R"({"role":"user","content":"Hi"}]})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"},
                                     {"x-ai-provider", "anthropic"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  EXPECT_THAT(upstream_body, HasSubstr("You are concise."));
  EXPECT_THAT(upstream_body, HasSubstr("Reply in English."));
  EXPECT_THAT(upstream_body, Not(HasSubstr("\"role\":\"system\"")));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// ── 18. Unknown depth-1 fields forwarded verbatim in OpenAI round-trip ───────
//
// Without x-ai-provider the encoder uses encodeInferenceBody, which must
// forward every depth-1 field that isn't in the extracted set (model, stream,
// messages, tools, sampling params) verbatim via passthrough_fields.
//
// This test uses:
//   response_format  — object passthrough
//   stream_options   — object passthrough
//   user             — string passthrough
//
// All three must appear unchanged in the upstream body; none must be absent.

TEST_P(AnthropicInferenceIntegrationTest, UnknownFieldsForwardedVerbatimInRoundTrip) {
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  const std::string body =
      R"({"model":"gpt-4o",)"
      R"("messages":[{"role":"user","content":"Hi"}],)"
      R"("response_format":{"type":"json_object"},)"
      R"("stream_options":{"include_usage":true},)"
      R"("user":"user-abc123"})";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/v1/chat/completions"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      body);

  waitForNextUpstreamRequest();
  const std::string upstream_body = upstream_request_->body().toString();

  // Object passthrough fields must be present verbatim.
  EXPECT_THAT(upstream_body, HasSubstr("\"response_format\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"type\":\"json_object\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"stream_options\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"include_usage\":true"));
  // String passthrough field must be present.
  EXPECT_THAT(upstream_body, HasSubstr("\"user\":\"user-abc123\""));
  // Extracted fields must still be present.
  EXPECT_THAT(upstream_body, HasSubstr("\"model\":\"gpt-4o\""));
  EXPECT_THAT(upstream_body, HasSubstr("\"role\":\"user\""));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
