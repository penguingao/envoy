#include <memory>
#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

TEST(JsonWithExtBufTest, InlinesSmallFieldsAndOffloadsTargetStrings) {
  // Sample OpenAI chat completion JSON payload
  const std::string raw_json = R"({
    "model": "gpt-4o",
    "temperature": 0.7,
    "stream": true,
    "messages": [
      {
        "role": "system",
        "content": "You are a helpful assistant."
      },
      {
        "role": "user",
        "content": "Analyze this 50-page document for security vulnerabilities..."
      }
    ]
  })";

  // Policy: offload any string under key "content"
  auto offload_policy = [](absl::string_view key, int /*depth*/, size_t /*token_start*/) {
    return key == "content";
  };

  auto result = JsonWithExtBuf::parse(raw_json, /*ext_buf=*/nullptr, offload_policy);
  ASSERT_TRUE(result.ok()) << result.status().message();

  std::unique_ptr<JsonWithExtBuf> doc = std::move(*result);
  const nlohmann::json& j = doc->json();

  // 1. Verify inlined top-level fields
  EXPECT_EQ(j["model"], "gpt-4o");
  EXPECT_DOUBLE_EQ(j["temperature"].get<double>(), 0.7);
  EXPECT_TRUE(j["stream"].get<bool>());

  // 2. Verify messages array structure
  ASSERT_TRUE(j.contains("messages"));
  ASSERT_TRUE(j["messages"].is_array());
  ASSERT_EQ(j["messages"].size(), 2);

  // 3. Verify message roles are inlined strings
  EXPECT_EQ(j["messages"][0]["role"], "system");
  EXPECT_EQ(j["messages"][1]["role"], "user");

  // 4. Verify message contents are converted to binary subtype offloaded references
  const nlohmann::json& sys_content = j["messages"][0]["content"];
  EXPECT_TRUE(JsonWithExtBuf::isOffloaded(sys_content));

  auto sys_loc = JsonWithExtBuf::getExtBufLocation(sys_content);
  ASSERT_TRUE(sys_loc.has_value());
  EXPECT_EQ(sys_loc->subtype, ExtBufLocation::kSubtypeOffloadedString);

  // Extract the content from raw_json using the location and verify slice match
  std::string extracted_sys(raw_json.data() + sys_loc->content_offset, sys_loc->content_length);
  EXPECT_EQ(extracted_sys, "You are a helpful assistant.");

  const nlohmann::json& user_content = j["messages"][1]["content"];
  EXPECT_TRUE(JsonWithExtBuf::isOffloaded(user_content));

  auto user_loc = JsonWithExtBuf::getExtBufLocation(user_content);
  ASSERT_TRUE(user_loc.has_value());
  std::string extracted_user(raw_json.data() + user_loc->content_offset, user_loc->content_length);
  EXPECT_EQ(extracted_user, "Analyze this 50-page document for security vulnerabilities...");
}

TEST(JsonWithExtBufTest, IncrementalChunkedFeed) {
  const std::string chunk1 = "{\"model\": \"gpt-4\", \"messages\": [{\"role\": \"user\", \"con";
  const std::string chunk2 = "tent\": \"hello world\"}]}";

  JsonWithExtBufParser parser(/*ext_buf=*/nullptr,
                              [](absl::string_view key, int, size_t) { return key == "content"; });

  EXPECT_TRUE(parser.feed(chunk1, /*is_last=*/false).ok());
  EXPECT_TRUE(parser.feed(chunk2, /*is_last=*/true).ok());

  auto result = parser.finalize();
  ASSERT_TRUE(result.ok());

  auto doc = std::move(*result);
  EXPECT_EQ(doc->json()["model"], "gpt-4");
  EXPECT_TRUE(JsonWithExtBuf::isOffloaded(doc->json()["messages"][0]["content"]));
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
