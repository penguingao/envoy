#include <string>

#include "envoy/extensions/filters/ai/auto_router/v3/auto_router.pb.h"

#include "source/extensions/filters/ai/auto_router/filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "test/mocks/server/factory_context.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {
namespace AutoRouter {
namespace {

using HttpFilters::AiProtocolManager::JsonWithExtBuf;
using testing::NiceMock;

// Builds a request the way the host adopts a parsed payload.
InferenceRequest makeRequest(nlohmann::json root) {
  JsonWithExtBuf payload;
  payload.setJson(std::move(root));
  return InferenceRequest(std::move(payload));
}

nlohmann::json userMessage(nlohmann::json content) {
  nlohmann::json message = nlohmann::json::object();
  message["role"] = "user";
  message["content"] = std::move(content);
  return message;
}

nlohmann::json payloadWith(nlohmann::json content) {
  nlohmann::json root = nlohmann::json::object();
  root["model"] = "auto";
  root["messages"] = nlohmann::json::array({userMessage(std::move(content))});
  return root;
}

class AutoRouterTest : public testing::Test {
public:
  Config makeConfig(const std::string& yaml) {
    AutoRouterProto proto;
    TestUtility::loadFromYaml(yaml, proto);
    return Config(proto, context_);
  }

  RequestSignals signalsFor(nlohmann::json root, uint32_t budget = 64 * 1024) {
    request_ = std::make_unique<InferenceRequest>(makeRequest(std::move(root)));
    return Filter::collectSignals(*request_, budget);
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  std::unique_ptr<InferenceRequest> request_;
};

// Inline prompt text is gathered and counted.
TEST_F(AutoRouterTest, CollectsInlinePrompt) {
  const RequestSignals signals = signalsFor(payloadWith("please write a haiku"));
  EXPECT_EQ(signals.prompt_bytes, 20);
  EXPECT_THAT(signals.inline_prompt, testing::HasSubstr("haiku"));
  EXPECT_FALSE(signals.has_tools);
  EXPECT_FALSE(signals.has_images);
}

// Offloaded content is counted but not read: its size is free, its text is not.
TEST_F(AutoRouterTest, CountsOffloadedPromptWithoutReadingIt) {
  const RequestSignals signals =
      signalsFor(payloadWith(JsonWithExtBuf::makeExternalRef({/*offset=*/0, /*length=*/50000})));
  EXPECT_EQ(signals.prompt_bytes, 50000);
  EXPECT_TRUE(signals.inline_prompt.empty());
}

TEST_F(AutoRouterTest, DetectsToolsAndImages) {
  nlohmann::json image_part = nlohmann::json::object();
  image_part["type"] = "image_url";
  image_part["image_url"] = nlohmann::json::object();
  image_part["image_url"]["url"] = JsonWithExtBuf::makeExternalRef({0, 1234});

  nlohmann::json text_part = nlohmann::json::object();
  text_part["type"] = "text";
  text_part["text"] = "describe this";

  nlohmann::json root = payloadWith(nlohmann::json::array({text_part, image_part}));
  root["tools"] = nlohmann::json::array({nlohmann::json::object()});

  const RequestSignals signals = signalsFor(std::move(root));
  EXPECT_TRUE(signals.has_tools);
  EXPECT_TRUE(signals.has_images);
  // The image bytes count toward the prompt without being matched against.
  EXPECT_EQ(signals.prompt_bytes, 1234 + 13);
}

// Only this many leading bytes are kept for matching; the count is unaffected.
TEST_F(AutoRouterTest, HonorsScanBudget) {
  const RequestSignals signals = signalsFor(payloadWith(std::string(1000, 'a')), /*budget=*/100);
  EXPECT_EQ(signals.prompt_bytes, 1000);
  EXPECT_LE(signals.inline_prompt.size(), 101);
}

TEST_F(AutoRouterTest, KeywordMatchIsCaseInsensitive) {
  const Config config = makeConfig(R"EOF(
routes:
- name: big-model
  keywords: ["PROVE", "theorem"]
default_route: small-model
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("Prove this Theorem"))), "big-model");
  EXPECT_EQ(config.pick(signalsFor(payloadWith("what is the weather"))), "small-model");
}

// More matched signals beats fewer, which is what "score" means here.
TEST_F(AutoRouterTest, HigherScoreWins) {
  const Config config = makeConfig(R"EOF(
routes:
- name: one-hit
  keywords: ["alpha"]
- name: two-hits
  keywords: ["alpha", "beta"]
default_route: fallback
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("alpha beta"))), "two-hits");
  EXPECT_EQ(config.pick(signalsFor(payloadWith("alpha only"))), "one-hit");
}

// Weight is the tie-breaker between routes that match equally well.
TEST_F(AutoRouterTest, WeightBreaksTies) {
  const Config config = makeConfig(R"EOF(
routes:
- name: light
  keywords: ["shared"]
  weight: 1
- name: heavy
  keywords: ["shared"]
  weight: 5
default_route: fallback
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("shared"))), "heavy");
}

// An equal score is broken by declaration order, so configuration order is the
// operator's last say.
TEST_F(AutoRouterTest, DeclarationOrderBreaksEqualScores) {
  const Config config = makeConfig(R"EOF(
routes:
- name: first
  keywords: ["same"]
- name: second
  keywords: ["same"]
default_route: fallback
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("same"))), "first");
}

// Structural predicates are hard filters: failing one rules the route out no
// matter how well its keywords match.
TEST_F(AutoRouterTest, StructuralPredicateExcludesRoute) {
  const Config config = makeConfig(R"EOF(
routes:
- name: vision
  keywords: ["describe"]
  structural:
    has_images: true
default_route: text-only
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("describe this"))), "text-only");

  nlohmann::json image_part = nlohmann::json::object();
  image_part["type"] = "image_url";
  image_part["image_url"] = nlohmann::json::object();
  image_part["image_url"]["url"] = "http://example.com/x.png";
  nlohmann::json text_part = nlohmann::json::object();
  text_part["type"] = "text";
  text_part["text"] = "describe this";
  EXPECT_EQ(config.pick(signalsFor(payloadWith(nlohmann::json::array({text_part, image_part})))),
            "vision");
}

// Size bounds are evaluated against offloaded content too, which is the point
// of counting it without reading it.
TEST_F(AutoRouterTest, PromptSizeBoundsUseOffloadedLength) {
  const Config config = makeConfig(R"EOF(
routes:
- name: long-context
  structural:
    min_prompt_bytes: 10000
default_route: standard
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith(JsonWithExtBuf::makeExternalRef({0, 50000})))),
            "long-context");
  EXPECT_EQ(config.pick(signalsFor(payloadWith("short"))), "standard");
}

TEST_F(AutoRouterTest, MaxPromptBytesBounds) {
  const Config config = makeConfig(R"EOF(
routes:
- name: small
  structural:
    max_prompt_bytes: 100
default_route: big
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("tiny"))), "small");
  EXPECT_EQ(config.pick(signalsFor(payloadWith(std::string(500, 'x')))), "big");
}

// A route whose preconditions hold but whose keywords do not still beats the
// default: the preconditions are themselves evidence.
TEST_F(AutoRouterTest, StructuralMatchWithoutKeywordsStillWins) {
  const Config config = makeConfig(R"EOF(
routes:
- name: tool-user
  keywords: ["unrelated"]
  structural:
    has_tools: true
default_route: fallback
)EOF");

  nlohmann::json root = payloadWith("hello");
  root["tools"] = nlohmann::json::array({nlohmann::json::object()});
  EXPECT_EQ(config.pick(signalsFor(std::move(root))), "tool-user");
}

// Regexes follow the usual StringMatcher contract: a full match over the
// scanned prompt, which is why the pattern is anchored with .* rather than
// searching for a fragment.
TEST_F(AutoRouterTest, RegexMatch) {
  const Config config = makeConfig(R"EOF(
routes:
- name: code
  regexes:
  - google_re2: {}
    regex: "(?s).*```[a-z]*.*"
default_route: prose
)EOF");

  EXPECT_EQ(config.pick(signalsFor(payloadWith("here is ```python code"))), "code");
  EXPECT_EQ(config.pick(signalsFor(payloadWith("just prose"))), "prose");
}

// With no default and no match, the router reports nothing and leaves the
// request alone.
TEST_F(AutoRouterTest, NoMatchAndNoDefaultReportsNothing) {
  const Config config = makeConfig(R"EOF(
routes:
- name: never
  keywords: ["zzz"]
)EOF");

  EXPECT_TRUE(config.pick(signalsFor(payloadWith("hello"))).empty());
}

TEST_F(AutoRouterTest, DefaultsForHeaderNameAndVerdict) {
  const Config config = makeConfig("routes: []");
  EXPECT_EQ(config.headerName().get(), "x-envoy-ai-route");
  EXPECT_EQ(config.verdict(), AutoRouterProto::SET_HEADER);
  EXPECT_EQ(config.maxScanBytes(), 64 * 1024);
}

} // namespace
} // namespace AutoRouter
} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
