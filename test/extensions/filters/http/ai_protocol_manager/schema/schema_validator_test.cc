#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/schema_validator.h"

#include "test/test_common/status_utility.h"

#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using ::Envoy::StatusHelpers::HasStatusCode;

class SchemaValidatorTest : public testing::Test {
public:
  // Parses `body` and validates it. nlohmann::json::parse is fine here: these
  // tests are about the walk, and a document with no offloaded values is what
  // most payloads are. The external-ref cases below build their nodes directly.
  absl::Status check(absl::string_view body, const FieldSchema& schema) {
    const nlohmann::json payload = nlohmann::json::parse(body, /*cb=*/nullptr,
                                                         /*allow_exceptions=*/false);
    // A test whose body does not parse is a broken test, not a validation result.
    EXPECT_FALSE(payload.is_discarded()) << "test body is not valid JSON: " << body;
    return validate(payload, schema);
  }

  // The message a violation produced, for asserting the client-visible contract.
  std::string message(absl::string_view body, const FieldSchema& schema) {
    return std::string(check(body, schema).message());
  }

  SchemaBuilder b_;
};

// Each kind accepts its own JSON type and rejects the others.
TEST_F(SchemaValidatorTest, StringKind) {
  const FieldSchema* schema = b_.object({{"f", b_.str()}});
  EXPECT_OK(check(R"({"f":"x"})", *schema));
  EXPECT_EQ(message(R"({"f":1})", *schema), "f: expected a string");
  EXPECT_EQ(message(R"({"f":[]})", *schema), "f: expected a string");
}

TEST_F(SchemaValidatorTest, NumberKind) {
  const FieldSchema* schema = b_.object({{"f", b_.number()}});
  EXPECT_OK(check(R"({"f":1.5})", *schema));
  EXPECT_OK(check(R"({"f":2})", *schema));
  EXPECT_EQ(message(R"({"f":"1"})", *schema), "f: expected a number");
}

TEST_F(SchemaValidatorTest, BoolKind) {
  const FieldSchema* schema = b_.object({{"f", b_.boolean()}});
  EXPECT_OK(check(R"({"f":true})", *schema));
  // The case a client hits by quoting a boolean, which the proxy acts on.
  EXPECT_EQ(message(R"({"f":"true"})", *schema), "f: expected a boolean");
}

TEST_F(SchemaValidatorTest, ArrayKind) {
  const FieldSchema* schema = b_.object({{"f", b_.array(b_.str())}});
  EXPECT_OK(check(R"({"f":["a","b"]})", *schema));
  EXPECT_EQ(message(R"({"f":{}})", *schema), "f: expected an array");
  EXPECT_EQ(message(R"({"f":["a",2]})", *schema), "f[1]: expected a string");
}

// An array whose element schema is null constrains the count and nothing else.
TEST_F(SchemaValidatorTest, ArrayWithUnconstrainedElements) {
  const FieldSchema* schema = b_.object({{"f", b_.array(nullptr)}});
  EXPECT_OK(check(R"({"f":["a",2,null,{},[]]})", *schema));
}

// An integer field takes 1024.0, because rejecting a request the upstream would
// accept is the worse failure. It still rejects a genuine fraction.
TEST_F(SchemaValidatorTest, IntKindAcceptsIntegralFloat) {
  const FieldSchema* schema = b_.object({{"f", b_.integer()}});
  EXPECT_OK(check(R"({"f":1024})", *schema));
  EXPECT_OK(check(R"({"f":1024.0})", *schema));
  EXPECT_EQ(message(R"({"f":1.5})", *schema), "f: expected an integer");
  EXPECT_EQ(message(R"({"f":"1"})", *schema), "f: expected an integer");
}

TEST_F(SchemaValidatorTest, ObjectKindRejectsNonObject) {
  const FieldSchema* schema = b_.object({{"f", b_.anyObject()}});
  EXPECT_OK(check(R"({"f":{"anything":1}})", *schema));
  EXPECT_EQ(message(R"({"f":[]})", *schema), "f: expected an object");
}

// A root-level failure has no field name to report, so it is named for what it is.
TEST_F(SchemaValidatorTest, RootFailureIsNamedPayload) {
  const FieldSchema* schema = b_.object({{"f", b_.str()}});
  EXPECT_EQ(message("[1,2,3]", *schema), "payload: expected an object");
  EXPECT_EQ(message(R"("just a string")", *schema), "payload: expected an object");
}

// A field the schema does not declare is forwarded untouched -- this is a proxy,
// not a validating gateway. Including names that collide with a nested object's
// declared fields, so the lookup is not accidentally global.
TEST_F(SchemaValidatorTest, UndeclaredFieldsPass) {
  const FieldSchema* schema =
      b_.object({{"declared", b_.str()}, {"nested", b_.object({{"inner", b_.str()}})}});
  EXPECT_OK(check(R"({
    "declared": "x",
    "nested": {"inner": "y", "inner_unknown": 1},
    "inner": 12345,
    "a": 1, "b": 2, "c": 3, "d": 4, "e": 5, "f": 6, "g": 7, "h": 8,
    "future_field": {"nested": [1, 2]}
  })",
                  *schema));
}

TEST_F(SchemaValidatorTest, RequiredFieldPresent) {
  const FieldSchema* schema = b_.object({{"f", Required, b_.str()}});
  EXPECT_OK(check(R"({"f":"x"})", *schema));
}

TEST_F(SchemaValidatorTest, RequiredFieldMissing) {
  const FieldSchema* schema = b_.object({{"f", Required, b_.str()}});
  EXPECT_EQ(message("{}", *schema), "f: required field is missing");
  // Present-but-undeclared keys must not be mistaken for the required one.
  EXPECT_EQ(message(R"({"other":"x"})", *schema), "f: required field is missing");
}

// null is not a way to satisfy a required field.
TEST_F(SchemaValidatorTest, RequiredFieldNull) {
  const FieldSchema* schema = b_.object({{"f", Required, b_.str()}});
  EXPECT_EQ(message(R"({"f":null})", *schema), "f: must not be null");
}

// An explicit null on an optional field means "unset" across the OpenAI surface,
// so it satisfies the field without reaching its kind check.
TEST_F(SchemaValidatorTest, OptionalFieldNullMeansUnset) {
  const FieldSchema* schema = b_.object({
      {"s", b_.str()},
      {"n", b_.number(0, 1)},
      {"i", b_.integer(1)},
      {"b", b_.boolean()},
      {"o", b_.anyObject()},
      {"a", b_.array(b_.str())},
      {"e", b_.str({"only"})},
  });
  EXPECT_OK(check(R"({"s":null,"n":null,"i":null,"b":null,"o":null,"a":null,"e":null})", *schema));
}

// The null rule belongs to the field edge, not to the value: an array element is
// not an optional field, so a null element is still a violation.
TEST_F(SchemaValidatorTest, NullArrayElementIsAViolation) {
  const FieldSchema* schema = b_.object({{"f", b_.array(b_.str())}});
  EXPECT_EQ(message(R"({"f":[null]})", *schema), "f[0]: expected a string");
}

TEST_F(SchemaValidatorTest, EnumConstraint) {
  const FieldSchema* schema = b_.object({{"f", b_.str({"a", "b"})}});
  EXPECT_OK(check(R"({"f":"a"})", *schema));
  EXPECT_OK(check(R"({"f":"b"})", *schema));
  EXPECT_EQ(message(R"({"f":"c"})", *schema), "f: value not permitted");
  // A prefix of a permitted value is not a permitted value.
  EXPECT_EQ(message(R"({"f":"ab"})", *schema), "f: value not permitted");
}

// Bounds are inclusive at both ends, and the message quotes the schema's own
// numbers -- which is the only thing besides the path that a message may carry.
TEST_F(SchemaValidatorTest, NumberBoundsAreInclusive) {
  const FieldSchema* schema = b_.object({{"f", b_.number(0.0, 2.0)}});
  EXPECT_OK(check(R"({"f":0})", *schema));
  EXPECT_OK(check(R"({"f":2})", *schema));
  EXPECT_EQ(message(R"({"f":-0.1})", *schema), "f: value must be at least 0");
  EXPECT_EQ(message(R"({"f":2.5})", *schema), "f: value must be at most 2");
}

TEST_F(SchemaValidatorTest, OneSidedBounds) {
  const FieldSchema* schema = b_.object({{"f", b_.integer(/*min_value=*/1)}});
  EXPECT_OK(check(R"({"f":1})", *schema));
  EXPECT_OK(check(R"({"f":999999})", *schema));
  EXPECT_EQ(message(R"({"f":0})", *schema), "f: value must be at least 1");
}

TEST_F(SchemaValidatorTest, MinItems) {
  const FieldSchema* one = b_.object({{"f", b_.array(b_.str(), /*min_items=*/1)}});
  EXPECT_OK(check(R"({"f":["a"]})", *one));
  EXPECT_EQ(message(R"({"f":[]})", *one), "f: must not be empty");

  const FieldSchema* two = b_.object({{"f", b_.array(b_.str(), /*min_items=*/2)}});
  EXPECT_EQ(message(R"({"f":["a"]})", *two), "f: must have at least 2 elements");
}

TEST_F(SchemaValidatorTest, OneOfAcceptsEveryAlternative) {
  const FieldSchema* schema = b_.object({{"f", b_.oneOf({b_.str(), b_.array(b_.str())})}});
  EXPECT_OK(check(R"({"f":"x"})", *schema));
  EXPECT_OK(check(R"({"f":["x","y"]})", *schema));
}

// The alternatives' own reasons are deliberately dropped: one message at the
// oneOf's own path, because a wall of alternative failures is noise.
TEST_F(SchemaValidatorTest, OneOfReportsOneMessage) {
  const FieldSchema* schema = b_.object({{"f", b_.oneOf({b_.str(), b_.array(b_.str())})}});
  EXPECT_EQ(message(R"({"f":123})", *schema), "f: value does not match any permitted form");
  EXPECT_EQ(message(R"({"f":[1]})", *schema), "f: value does not match any permitted form");
}

// A failed alternative must not leave anything behind on the path. The first
// alternative here descends and appends ".a" before failing, and the second then
// succeeds; a later field's violation has to still report its own bare path.
//
// The field is named to sort after "f": nlohmann objects are std::map-backed, so
// the walk visits keys in alphabetical order and an earlier-sorting field would
// report its own violation first.
TEST_F(SchemaValidatorTest, FailedAlternativeDoesNotCorruptThePath) {
  const FieldSchema* schema = b_.object({
      {"f",
       b_.oneOf({b_.object({{"a", Required, b_.str()}}), b_.object({{"b", Required, b_.str()}})})},
      {"z_after", b_.str()},
  });

  // "f" matches the second alternative only, and "z_after" is the violation.
  EXPECT_EQ(message(R"({"f":{"b":"ok"},"z_after":2})", *schema), "z_after: expected a string");
  // Neither alternative matches.
  EXPECT_EQ(message(R"({"f":{"c":"ok"},"z_after":"ok"})", *schema),
            "f: value does not match any permitted form");
}

// AnyJson takes any shape and is never descended into, which is what keeps
// caller-authored JSON Schema out of scope.
TEST_F(SchemaValidatorTest, AnyJsonAcceptsEverything) {
  const FieldSchema* schema = b_.object({{"f", b_.anyJson()}});
  EXPECT_OK(check(R"({"f":{"deeply":{"nested":[1,2,{"x":"y"}]}}})", *schema));
  EXPECT_OK(check(R"({"f":[1,2,3]})", *schema));
  EXPECT_OK(check(R"({"f":"scalar"})", *schema));
  EXPECT_OK(check(R"({"f":42})", *schema));
  EXPECT_OK(check(R"({"f":null})", *schema));
}

TEST_F(SchemaValidatorTest, PathsAreBuiltForNestedShapes) {
  const FieldSchema* role = b_.object({{"role", b_.str({"user"})}});
  const FieldSchema* schema = b_.object({
      {"messages", b_.array(role)},
      {"a", b_.object({{"b", b_.object({{"c", b_.str()}})}})},
      {"grid", b_.array(b_.array(b_.str()))},
  });

  EXPECT_EQ(message(R"({"messages":[{"role":"user"},{"role":"user"},{"role":"wizard"}]})", *schema),
            "messages[2].role: value not permitted");
  EXPECT_EQ(message(R"({"a":{"b":{"c":1}}})", *schema), "a.b.c: expected a string");
  EXPECT_EQ(message(R"({"grid":[["a"],[1]]})", *schema), "grid[1][0]: expected a string");
}

// One reason to reject, not an audit report: the walk stops at the first
// violation and never reaches the later ones.
TEST_F(SchemaValidatorTest, FirstViolationWins) {
  const FieldSchema* schema = b_.object({{"a", b_.array(b_.str())}});
  EXPECT_EQ(message(R"({"a":[1,2,3]})", *schema), "a[0]: expected a string");
}

// The property that keeps prompt content out of responses, access logs and stats:
// a violation message carries the path and the schema's own expectation, and
// nothing whatsoever from the payload.
TEST_F(SchemaValidatorTest, ViolationMessageNeverEchoesThePayload) {
  constexpr absl::string_view kSecret = "SUPERSECRETPROMPT";
  const FieldSchema* schema = b_.object({
      {"typed", b_.integer()},
      {"enumerated", b_.str({"permitted"})},
      {"bounded", b_.number(0.0, 1.0)},
      {"required_field", Required, b_.str()},
      {"nested", b_.object({{"inner", b_.str()}})},
  });

  const std::string bodies[] = {
      // Wrong type, value is the secret.
      R"({"typed":"SUPERSECRETPROMPT","required_field":"x"})",
      // Enum non-match, value is the secret.
      R"({"enumerated":"SUPERSECRETPROMPT","required_field":"x"})",
      // Out of range, rendered as a number.
      R"({"bounded":999.5,"required_field":"x"})",
      // Missing required field, with the secret as an undeclared key AND value.
      R"({"SUPERSECRETPROMPT":"SUPERSECRETPROMPT"})",
      // Nested wrong type.
      R"({"nested":{"inner":["SUPERSECRETPROMPT"]},"required_field":"x"})",
  };

  for (const std::string& body : bodies) {
    const absl::Status status = check(body, *schema);
    ASSERT_THAT(status, HasStatusCode(absl::StatusCode::kInvalidArgument)) << body;
    EXPECT_FALSE(absl::StrContains(status.message(), kSecret))
        << "message leaked payload content: " << status.message();
    // 999.5 is the payload's value, not the schema's bound.
    EXPECT_FALSE(absl::StrContains(status.message(), "999.5"))
        << "message leaked payload content: " << status.message();
  }
}

// An oversized string is not in the DOM at all -- it is a binary node holding an
// ExternalRef -- so a string check has to recognize that shape as a string.
TEST_F(SchemaValidatorTest, ExternalRefSatisfiesAStringField) {
  const FieldSchema* schema = b_.object({{"content", b_.offloadableStr(StreamOrder::Prompt)}});
  nlohmann::json payload = nlohmann::json::object();
  payload["content"] = JsonWithExtBuf::makeExternalRef({/*offset=*/10, /*length=*/4096});

  EXPECT_OK(validate(payload, *schema));
}

// A value the proxy has to compare against a list is one the proxy has to be able
// to read. An offloaded value is longer than the inline threshold and every
// permitted value is far shorter, so it matches none of them -- decided without
// reading the buffer. A schema never marks an enum field offloadable, so this is
// a defensive branch rather than a reachable one.
TEST_F(SchemaValidatorTest, ExternalRefCannotSatisfyAnEnum) {
  const FieldSchema* schema = b_.object({{"role", b_.str({"user", "assistant"})}});
  nlohmann::json payload = nlohmann::json::object();
  payload["role"] = JsonWithExtBuf::makeExternalRef({/*offset=*/0, /*length=*/2048});

  EXPECT_EQ(std::string(validate(payload, *schema).message()), "role: value not permitted");
}

TEST_F(SchemaValidatorTest, ExternalRefIsNotANumberObjectOrArray) {
  const nlohmann::json ref = JsonWithExtBuf::makeExternalRef({/*offset=*/0, /*length=*/2048});

  const FieldSchema* number = b_.object({{"f", b_.number()}});
  const FieldSchema* object = b_.object({{"f", b_.anyObject()}});
  const FieldSchema* array = b_.object({{"f", b_.array(b_.str())}});
  const FieldSchema* any = b_.object({{"f", b_.anyJson()}});

  nlohmann::json payload = nlohmann::json::object();
  payload["f"] = ref;

  EXPECT_EQ(std::string(validate(payload, *number).message()), "f: expected a number");
  EXPECT_EQ(std::string(validate(payload, *object).message()), "f: expected an object");
  EXPECT_EQ(std::string(validate(payload, *array).message()), "f: expected an array");
  EXPECT_OK(validate(payload, *any));
}

// A binary node written by something other than the offload path must not be
// mistaken for an offloaded string.
TEST_F(SchemaValidatorTest, UntaggedBinaryNodeIsNotAString) {
  const FieldSchema* schema = b_.object({{"f", b_.str()}});
  nlohmann::json payload = nlohmann::json::object();
  payload["f"] = nlohmann::json::binary(std::vector<std::uint8_t>{1, 2, 3});

  EXPECT_EQ(std::string(validate(payload, *schema).message()), "f: expected a string");
}

// The builder derives the required-field count and shares nodes between parents
// rather than copying them, which is what lets one content-part schema serve two
// fields.
TEST_F(SchemaValidatorTest, BuilderDerivesStateAndSharesNodes) {
  const FieldSchema* shared = b_.object({{"inner", Required, b_.str()}});
  const FieldSchema* schema = b_.object({
      {"a", Required, shared},
      {"b", shared},
      {"c", b_.str()},
  });

  EXPECT_EQ(schema->required_field_count, 1);
  EXPECT_EQ(schema->fields.size(), 3);
  EXPECT_EQ(schema->fields.at("a").schema, schema->fields.at("b").schema);
  EXPECT_TRUE(schema->fields.at("a").required);
  EXPECT_FALSE(schema->fields.at("b").required);
  EXPECT_EQ(shared->required_field_count, 1);

  // Both parents enforce the shared subtree.
  EXPECT_EQ(message(R"({"a":{},"b":{"inner":"x"}})", *schema),
            "a.inner: required field is missing");
  EXPECT_EQ(message(R"({"a":{"inner":"x"},"b":{}})", *schema),
            "b.inner: required field is missing");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
