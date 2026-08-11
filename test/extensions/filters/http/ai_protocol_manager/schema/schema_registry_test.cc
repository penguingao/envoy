#include "source/extensions/filters/http/ai_protocol_manager/schema/schema_registry.h"

#include "test/test_common/status_utility.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Adding a schema to the proto without adding a table for it must fail here
// rather than silently forwarding every payload unvalidated. Generated proto
// enums carry sentinel enumerators, so -Wswitch cannot catch this; the descriptor
// can.
TEST(SchemaRegistryTest, EverySchemaExceptUnspecifiedResolves) {
  const auto* descriptor = PerRouteProto::Schema_descriptor();
  ASSERT_NE(descriptor, nullptr);
  // Guards against the loop below passing because the enum lost its values.
  EXPECT_GE(descriptor->value_count(), 2);

  for (int i = 0; i < descriptor->value_count(); ++i) {
    const auto value = static_cast<Schema>(descriptor->value(i)->number());
    SCOPED_TRACE(descriptor->value(i)->name());
    if (value == PerRouteProto::UNSPECIFIED) {
      EXPECT_EQ(requestSchemaFor(value), nullptr);
      EXPECT_EQ(responseSchemaFor(value), nullptr);
    } else {
      EXPECT_NE(requestSchemaFor(value), nullptr);
      EXPECT_NE(responseSchemaFor(value), nullptr);
    }
  }
}

// A route that declared nothing has nothing to hold its payload to, and the
// filter reads that null as "not an AI endpoint".
TEST(SchemaRegistryTest, UnspecifiedHasNoSchema) {
  EXPECT_EQ(requestSchemaFor(PerRouteProto::UNSPECIFIED), nullptr);
  EXPECT_EQ(responseSchemaFor(PerRouteProto::UNSPECIFIED), nullptr);
}

TEST(SchemaRegistryTest, OpenAiChatCompletionsResolvesAndIsNamed) {
  const PayloadSchema* request = requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS);
  ASSERT_NE(request, nullptr);
  EXPECT_EQ(request->name(), "openai_chat_completions");
  EXPECT_FALSE(request->offloadPlan().streamOrder().empty());
}

// Canonical is the OpenAI shape for now, so normalization is an identity
// transform. Pointer identity, so a future divergence has to be deliberate.
TEST(SchemaRegistryTest, CanonicalRequestSchemaIsOpenAiChatCompletions) {
  EXPECT_EQ(&canonicalRequestSchema(), requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS));
}

// Built once and handed out, not rebuilt per call -- which is what makes it safe
// for the filter to cache the pointer for a stream.
TEST(SchemaRegistryTest, SchemasAreBuiltOnce) {
  EXPECT_EQ(requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS),
            requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS));
  EXPECT_EQ(responseSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS),
            responseSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS));
}

// Validating does not mutate the shared tables, so two streams cannot affect each
// other's answer.
TEST(SchemaRegistryTest, ValidationIsRepeatable) {
  const PayloadSchema* schema = requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS);
  ASSERT_NE(schema, nullptr);

  const nlohmann::json valid =
      nlohmann::json::parse(R"({"model":"m","messages":[{"role":"user","content":"x"}]})");
  const nlohmann::json invalid = nlohmann::json::parse(R"({"messages":[]})");

  for (int i = 0; i < 3; ++i) {
    EXPECT_OK(schema->validate(valid));
    EXPECT_EQ(std::string(schema->validate(invalid).message()), "messages: must not be empty");
  }
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
