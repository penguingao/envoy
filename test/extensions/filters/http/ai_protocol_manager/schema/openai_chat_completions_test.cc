#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "envoy/common/exception.h"

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf_parser.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/openai_chat_completions.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/tree_payload_schema.h"

#include "test/test_common/status_utility.h"

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using ::Envoy::StatusHelpers::HasStatusCode;

class OpenAiChatCompletionsTest : public testing::Test {
public:
  OpenAiChatCompletionsTest()
      : request_("openai_chat_completions", buildOpenAiChatCompletionsRequestSchema,
                 openAiChatCompletionsStreamOrder),
        response_("openai_chat_completions_response", buildOpenAiChatCompletionsResponseSchema) {}

  absl::Status check(absl::string_view body) {
    const nlohmann::json payload = nlohmann::json::parse(body, /*cb=*/nullptr,
                                                         /*allow_exceptions=*/false);
    EXPECT_FALSE(payload.is_discarded()) << "test body is not valid JSON: " << body;
    return request_.validate(payload);
  }

  std::string message(absl::string_view body) { return std::string(check(body).message()); }

  // Parses `body` the way the filter does, so an oversized string becomes an
  // external reference rather than a string node. That is the shape the validator
  // actually meets in production, and a hand-built DOM would not exercise it.
  absl::StatusOr<JsonWithExtBuf> parseAsFilterWould(absl::string_view body) {
    JsonWithExtBufParser parser(JsonWithExtBufParser::Config{});
    RETURN_IF_NOT_OK(parser.feed(body, /*end_stream=*/true));
    return parser.takeDocument();
  }

  TreePayloadSchema request_;
  TreePayloadSchema response_;
};

TEST_F(OpenAiChatCompletionsTest, MinimalValidRequest) {
  EXPECT_OK(check(R"({"model":"gpt-4","messages":[{"role":"user","content":"hi"}]})"));
}

// Every declared field at once, with valid values. Doubles as documentation of
// what this schema covers.
TEST_F(OpenAiChatCompletionsTest, KitchenSinkRequest) {
  EXPECT_OK(check(R"({
    "model": "gpt-4o",
    "messages": [
      {"role": "developer", "content": "be terse"},
      {"role": "system", "content": "you are a proxy"},
      {"role": "user", "content": [
        {"type": "text", "text": "what is in this image?"},
        {"type": "image_url", "image_url": {"url": "https://example.com/a.png", "detail": "high"}},
        {"type": "input_audio", "input_audio": {"data": "AAAA", "format": "wav"}},
        {"type": "file", "file": {"file_id": "file-123"}}
      ]},
      {"role": "assistant", "content": null, "refusal": null, "tool_calls": [
        {"id": "call_1", "type": "function",
         "function": {"name": "get_weather", "arguments": "{\"city\":\"Paris\"}"}}
      ]},
      {"role": "tool", "tool_call_id": "call_1", "content": "18C"},
      {"role": "assistant", "content": "It is 18C.", "name": "assistant-1"}
    ],
    "temperature": 0.7,
    "top_p": 0.9,
    "presence_penalty": 0.5,
    "frequency_penalty": -0.5,
    "n": 1,
    "max_tokens": 256,
    "max_completion_tokens": 512,
    "top_logprobs": 5,
    "seed": 42,
    "logprobs": true,
    "stream": true,
    "stream_options": {"include_usage": true},
    "stop": ["\n\n"],
    "tools": [
      {"type": "function", "function": {
        "name": "get_weather",
        "description": "Look up the weather",
        "strict": true,
        "parameters": {
          "type": "object",
          "properties": {"city": {"type": "string", "description": "City name"}},
          "required": ["city"]
        }
      }}
    ],
    "tool_choice": "auto",
    "parallel_tool_calls": false,
    "response_format": {"type": "json_schema", "json_schema": {
      "name": "reply", "description": "a reply", "strict": true,
      "schema": {"type": "object", "properties": {"answer": {"type": "string"}}}
    }},
    "prediction": {"type": "content", "content": "draft text"},
    "store": false,
    "user": "user-123",
    "modalities": ["text", "audio"],
    "audio": {"voice": "alloy", "format": "wav"},
    "logit_bias": {"1234": -100},
    "metadata": {"tenant": "acme"},
    "service_tier": "auto",
    "reasoning_effort": "medium"
  })"));
}

// The string and multi-part forms of content, and the null form on a
// tool-calling assistant turn.
TEST_F(OpenAiChatCompletionsTest, EveryContentForm) {
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"a string"}]})"));
  EXPECT_OK(check(R"({"model":"m","messages":[
    {"role":"user","content":[{"type":"text","text":"parts"}]}]})"));
  EXPECT_OK(check(R"({"model":"m","messages":[
    {"role":"assistant","content":null,
     "tool_calls":[{"id":"c1","type":"function","function":{"name":"f","arguments":"{}"}}]}]})"));
  // Absent entirely is also fine -- content is optional.
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"assistant"}]})"));
}

// The oneOf swallows the inner reason by design, so assert the message shape
// deliberately rather than discovering it later.
TEST_F(OpenAiChatCompletionsTest, MalformedContentPartReportsAtTheContentPath) {
  EXPECT_EQ(message(R"({"model":"m","messages":[{"role":"user","content":[{"text":"no type"}]}]})"),
            "messages[0].content: value does not match any permitted form");
  EXPECT_EQ(message(R"({"model":"m","messages":[{"role":"user","content":42}]})"),
            "messages[0].content: value does not match any permitted form");
}

// A caller's JSON Schema rides through untouched, however it is shaped.
TEST_F(OpenAiChatCompletionsTest, ToolParametersAreOpaque) {
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "tools":[{"type":"function","function":{"name":"f","parameters":{
      "type":"object","properties":{"a":{"type":"array","items":{"type":"string"}}},
      "additionalProperties":false,"$defs":{"x":{"enum":[1,2,3]}}}}}]})"));
  // Even a shape that is not a JSON Schema at all.
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "tools":[{"type":"function","function":{"name":"f","parameters":"not an object"}}]})"));
}

TEST_F(OpenAiChatCompletionsTest, ToolFunctionNameIsRequired) {
  EXPECT_EQ(message(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "tools":[{"type":"function","function":{"description":"no name"}}]})"),
            "tools[0].function.name: required field is missing");
}

// The client-visible contract: each of these has to keep saying exactly this.
TEST_F(OpenAiChatCompletionsTest, RejectionMessages) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"({"messages":[{"role":"user","content":"x"}]})", "model: required field is missing"},
      {R"({"model":"m"})", "messages: required field is missing"},
      {R"({"model":"m","messages":[]})", "messages: must not be empty"},
      {R"({"model":123,"messages":[{"role":"user","content":"x"}]})", "model: expected a string"},
      {R"({"model":"m","messages":[{"content":"x"}]})",
       "messages[0].role: required field is missing"},
      {R"({"model":"m","messages":"not an array"})", "messages: expected an array"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"temperature":2.5})",
       "temperature: value must be at most 2"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"top_p":-0.1})",
       "top_p: value must be at least 0"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"n":0})",
       "n: value must be at least 1"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"top_logprobs":21})",
       "top_logprobs: value must be at most 20"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"stream":"true"})",
       "stream: expected a boolean"},
      {R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":1.5})",
       "max_tokens: expected an integer"},
      {"[1,2,3]", "payload: expected an object"},
      {R"("a string")", "payload: expected an object"},
      {"42", "payload: expected an object"},
  };

  for (const auto& [body, expected] : cases) {
    SCOPED_TRACE(body);
    EXPECT_EQ(message(body), expected);
  }
}

// The object form of tool_choice, which the enum alternative does not cover.
TEST_F(OpenAiChatCompletionsTest, ToolChoiceObjectForm) {
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "tool_choice":{"type":"function","function":{"name":"f"}}})"));
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "tool_choice":"required"})"));
}

// Both documented forms of stop.
TEST_F(OpenAiChatCompletionsTest, StopTakesEitherForm) {
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],"stop":"\n"})"));
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],
    "stop":["\n","END"]})"));
  EXPECT_OK(check(R"({"model":"m","messages":[{"role":"user","content":"x"}],"stop":null})"));
}

// A field OpenAI ships after this Envoy was built must not be rejected. This is
// what the undeclared-fields-pass decision buys, and why it is a decision.
TEST_F(OpenAiChatCompletionsTest, UnknownFieldsAreForwarded) {
  EXPECT_OK(check(R"({
    "model": "gpt-5",
    "messages": [{"role": "user", "content": "x"}],
    "verbosity": "low",
    "safety_identifier": "abc",
    "prompt_cache_key": "k",
    "web_search_options": {"search_context_size": "medium"},
    "future_field": {"nested": [1, 2, {"deep": true}]}
  })"));
}

// A deprecated field that real clients still send.
TEST_F(OpenAiChatCompletionsTest, DeprecatedFieldsStillPass) {
  EXPECT_OK(check(R"({"model":"m","messages":[
    {"role":"function","name":"f","content":"result"},
    {"role":"assistant","function_call":{"name":"f","arguments":"{}"}}]})"));
}

// An oversized prompt is left in the external buffer, so the validator meets a
// binary node where a string is declared. It has to accept it -- otherwise every
// large prompt would be a false rejection, which is the whole point of tying the
// schema to the offload design.
TEST_F(OpenAiChatCompletionsTest, OffloadedPromptStillValidates) {
  const std::string prompt(4096, 'p');
  const absl::StatusOr<JsonWithExtBuf> doc = parseAsFilterWould(absl::StrCat(
      R"({"model":"gpt-4","messages":[{"role":"user","content":")", prompt, R"("}]})"));
  ASSERT_OK(doc);

  // The prompt really did leave the DOM.
  const nlohmann::json& content = doc->json()["messages"][0]["content"];
  ASSERT_TRUE(JsonWithExtBuf::isExternalRef(content));
  // ...while the short metadata stayed inline, which is what lets `role` still be
  // checked against its permitted values.
  EXPECT_TRUE(doc->json()["messages"][0]["role"].is_string());
  EXPECT_TRUE(doc->json()["model"].is_string());

  EXPECT_OK(request_.validate(doc->json()));
}

// The same, for a multi-part content array.
TEST_F(OpenAiChatCompletionsTest, OffloadedContentPartStillValidates) {
  const std::string prompt(4096, 'q');
  const absl::StatusOr<JsonWithExtBuf> doc =
      parseAsFilterWould(absl::StrCat(R"({"model":"gpt-4","messages":[{"role":"user","content":[)",
                                      R"({"type":"text","text":")", prompt, R"("}]}]})"));
  ASSERT_OK(doc);

  const nlohmann::json& text = doc->json()["messages"][0]["content"][0]["text"];
  ASSERT_TRUE(JsonWithExtBuf::isExternalRef(text));
  EXPECT_OK(request_.validate(doc->json()));
}

// Only free text is declared offloadable, and every value-constrained field is
// inline by construction.
TEST_F(OpenAiChatCompletionsTest, OffloadPlanCoversOnlyFreeText) {
  const OffloadPlan& plan = request_.offloadPlan();

  EXPECT_TRUE(plan.isOffloadable("messages[].content"));
  EXPECT_TRUE(plan.isOffloadable("messages[].content[].text"));
  EXPECT_TRUE(plan.isOffloadable("messages[].content[].image_url.url"));
  EXPECT_TRUE(plan.isOffloadable("messages[].tool_calls[].function.arguments"));
  EXPECT_TRUE(plan.isOffloadable("tools[].function.description"));
  EXPECT_TRUE(plan.isOffloadable("prediction.content"));

  // Everything the schema constrains by value has to stay readable.
  EXPECT_FALSE(plan.isOffloadable("model"));
  EXPECT_FALSE(plan.isOffloadable("messages[].role"));
  EXPECT_FALSE(plan.isOffloadable("messages[].content[].type"));
  EXPECT_FALSE(plan.isOffloadable("response_format.type"));
  EXPECT_FALSE(plan.isOffloadable("tools[].function.name"));
}

// Prompts stream before tool payloads. The order is the declared list verbatim,
// and every path in it having survived OffloadPlan's assertion is itself proof
// that each names a real offloadable field.
TEST_F(OpenAiChatCompletionsTest, PromptsStreamBeforeTools) {
  const absl::Span<const std::string> order = request_.offloadPlan().streamOrder();

  const auto index_of = [&order](absl::string_view path) {
    const auto it = std::find(order.begin(), order.end(), path);
    EXPECT_NE(it, order.end()) << path;
    return std::distance(order.begin(), it);
  };

  EXPECT_LT(index_of("messages[].content"), index_of("messages[].tool_calls[].function.arguments"));
  EXPECT_LT(index_of("messages[].content[].text"), index_of("tools[].function.description"));
  EXPECT_LT(index_of("prediction.content"), index_of("tools[].function.description"));
}

// The declared order names every offloadable field, so none of them is left to
// fall to the end by omission.
TEST_F(OpenAiChatCompletionsTest, StreamOrderNamesEveryOffloadableField) {
  EXPECT_EQ(request_.offloadPlan().streamOrder().size(), openAiChatCompletionsStreamOrder().size());
}

// The response schema is deliberately empty, but not dead: it still says the
// response is an object.
TEST_F(OpenAiChatCompletionsTest, ResponseSchemaAcceptsAnyObject) {
  const nlohmann::json response = nlohmann::json::parse(R"({
    "id": "chatcmpl-1", "object": "chat.completion", "created": 1,
    "model": "gpt-4", "choices": [{"index": 0,
      "message": {"role": "assistant", "content": "hi"}, "finish_reason": "stop"}],
    "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}
  })");
  EXPECT_OK(response_.validate(response));

  EXPECT_THAT(response_.validate(nlohmann::json::array()),
              HasStatusCode(absl::StatusCode::kInvalidArgument));
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
