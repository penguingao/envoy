#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/inference_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Builds a request around `root`, the way the filter adopts a parsed document.
InferenceRequest makeRequest(nlohmann::json root) {
  JsonWithExtBuf payload;
  payload.setJson(std::move(root));
  return InferenceRequest(std::move(payload));
}

nlohmann::json chatCompletionsPayload() {
  nlohmann::json message = nlohmann::json::object();
  message["role"] = "user";
  message["content"] = "hello";

  nlohmann::json root = nlohmann::json::object();
  root["model"] = "gpt-4";
  root["messages"] = nlohmann::json::array({message});
  root["stream"] = true;
  root["max_tokens"] = 256;
  return root;
}

TEST(InferenceRequestTest, ReadsCommonFields) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());

  ASSERT_TRUE(request.model().has_value());
  EXPECT_EQ(*request.model(), "gpt-4");
  ASSERT_TRUE(request.stream().has_value());
  EXPECT_TRUE(*request.stream());
  ASSERT_TRUE(request.maxTokens().has_value());
  EXPECT_EQ(*request.maxTokens(), 256);

  ASSERT_NE(request.messages(), nullptr);
  EXPECT_EQ(request.messages()->size(), 1);
  EXPECT_EQ(request.tools(), nullptr);
}

// A field that is absent reads as nullopt rather than failing.
TEST(InferenceRequestTest, AbsentFieldsAreNullopt) {
  InferenceRequest request = makeRequest(nlohmann::json::object());

  EXPECT_FALSE(request.model().has_value());
  EXPECT_FALSE(request.stream().has_value());
  EXPECT_FALSE(request.maxTokens().has_value());
  EXPECT_EQ(request.messages(), nullptr);
  EXPECT_EQ(request.tools(), nullptr);
}

// So does a field of the wrong type: the schema layer rejects a malformed
// payload, so these accessors do not have to fail on one.
TEST(InferenceRequestTest, WrongTypedFieldsAreNullopt) {
  nlohmann::json root = nlohmann::json::object();
  root["model"] = 7;
  root["stream"] = "yes";
  root["max_tokens"] = "many";
  root["messages"] = "not an array";
  InferenceRequest request = makeRequest(std::move(root));

  EXPECT_FALSE(request.model().has_value());
  EXPECT_FALSE(request.stream().has_value());
  EXPECT_FALSE(request.maxTokens().has_value());
  EXPECT_EQ(request.messages(), nullptr);
}

// A payload that is not an object at all does not crash the accessors.
TEST(InferenceRequestTest, NonObjectRoot) {
  InferenceRequest request = makeRequest(nlohmann::json::array({1, 2}));

  EXPECT_FALSE(request.model().has_value());
  EXPECT_EQ(request.messages(), nullptr);
}

// The dirty flag is what the verbatim-replay fast path keys on.
TEST(InferenceRequestTest, StartsClean) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  EXPECT_FALSE(request.dirty());

  // Reading, however deeply, leaves it clean.
  EXPECT_EQ(*request.model(), "gpt-4");
  EXPECT_NE(request.messages(), nullptr);
  EXPECT_EQ(request.json().at("model"), "gpt-4");
  EXPECT_FALSE(request.dirty());
}

// Taking the mutable reference marks the payload dirty even without a write:
// a false positive costs the slow path, a false negative would forward stale
// bytes.
TEST(InferenceRequestTest, MutableAccessMarksDirty) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  request.mutableJson();
  EXPECT_TRUE(request.dirty());
}

TEST(InferenceRequestTest, SetModelMarksDirtyAndReplacesValue) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  request.setModel("gpt-4o-mini");

  EXPECT_TRUE(request.dirty());
  ASSERT_TRUE(request.model().has_value());
  EXPECT_EQ(*request.model(), "gpt-4o-mini");
}

// Routing to a different model is the motivating mutation, so it must survive
// into the DOM the emitter will serialize.
TEST(InferenceRequestTest, SetModelIsVisibleInJson) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  request.setModel("claude");
  EXPECT_EQ(request.json().at("model").get<std::string>(), "claude");
}

TEST(InferenceRequestTest, NoOffloadedRangesWhenAllInline) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  EXPECT_TRUE(request.offloadedRanges().empty());
  EXPECT_EQ(request.offloadedBytes(), 0);
}

// The point of the accounting: a prompt's size is known from the references,
// with no buffer and no reads.
TEST(InferenceRequestTest, CountsOffloadedRanges) {
  nlohmann::json first = nlohmann::json::object();
  first["role"] = "system";
  first["content"] = JsonWithExtBuf::makeExternalRef({/*offset=*/0, /*length=*/1000});
  nlohmann::json second = nlohmann::json::object();
  second["role"] = "user";
  second["content"] = JsonWithExtBuf::makeExternalRef({/*offset=*/1000, /*length=*/2500});

  nlohmann::json root = nlohmann::json::object();
  root["model"] = "gpt-4";
  root["messages"] = nlohmann::json::array({first, second});
  InferenceRequest request = makeRequest(std::move(root));

  const std::vector<JsonWithExtBuf::ExternalRef> ranges = request.offloadedRanges();
  ASSERT_EQ(ranges.size(), 2);
  EXPECT_EQ(ranges[0].offset, 0);
  EXPECT_EQ(ranges[0].length, 1000);
  EXPECT_EQ(ranges[1].offset, 1000);
  EXPECT_EQ(ranges[1].length, 2500);
  EXPECT_EQ(request.offloadedBytes(), 3500);
}

// References are found wherever they sit, not only directly under messages.
TEST(InferenceRequestTest, FindsNestedOffloadedRanges) {
  nlohmann::json part = nlohmann::json::object();
  part["type"] = "text";
  part["text"] = JsonWithExtBuf::makeExternalRef({0, 10});

  nlohmann::json image = nlohmann::json::object();
  image["type"] = "image_url";
  image["image_url"] = nlohmann::json::object();
  image["image_url"]["url"] = JsonWithExtBuf::makeExternalRef({10, 90});

  nlohmann::json message = nlohmann::json::object();
  message["role"] = "user";
  message["content"] = nlohmann::json::array({part, image});

  nlohmann::json root = nlohmann::json::object();
  root["messages"] = nlohmann::json::array({message});
  InferenceRequest request = makeRequest(std::move(root));

  EXPECT_EQ(request.offloadedRanges().size(), 2);
  EXPECT_EQ(request.offloadedBytes(), 100);
}

// Inspecting offloaded content must not itself dirty the payload, or every
// request carrying a large prompt would lose the fast path.
TEST(InferenceRequestTest, OffloadAccountingLeavesPayloadClean) {
  nlohmann::json root = nlohmann::json::object();
  root["messages"] = nlohmann::json::array({JsonWithExtBuf::makeExternalRef({0, 42})});
  InferenceRequest request = makeRequest(std::move(root));

  EXPECT_EQ(request.offloadedBytes(), 42);
  EXPECT_FALSE(request.dirty());
}

// The document can be handed back for serialization.
TEST(InferenceRequestTest, ReleaseYieldsDocument) {
  InferenceRequest request = makeRequest(chatCompletionsPayload());
  JsonWithExtBuf payload = std::move(request).release();
  EXPECT_EQ(payload.json().at("model").get<std::string>(), "gpt-4");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
