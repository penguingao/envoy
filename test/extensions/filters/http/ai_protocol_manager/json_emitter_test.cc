#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/json_emitter.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using StatusHelpers::IsOk;
using StatusHelpers::StatusCodeIs;

// Stands in for the external buffer: a range resolves to the slice of `store`
// it names, which is what the BufferManager will later read back.
struct FakeStore {
  std::string bytes;

  absl::string_view resolve(const JsonWithExtBuf::ExternalRef& ref) const {
    EXPECT_LE(ref.offset + ref.length, bytes.size());
    return absl::string_view(bytes).substr(ref.offset, ref.length);
  }
};

// Drives the emitter to completion, splicing ranges out of `store`, and returns
// the assembled document. `flush_bytes` controls how finely the emitter chops
// its output; the result must not depend on it.
absl::StatusOr<std::string> emit(const nlohmann::json& root, const FakeStore& store,
                                 uint32_t flush_bytes = JsonEmitter::DefaultFlushBytes) {
  JsonEmitter emitter(root, flush_bytes);
  std::string out;
  while (true) {
    switch (emitter.next()) {
    case JsonEmitter::State::Text:
      out.append(emitter.text());
      break;
    case JsonEmitter::State::Range:
      out.append(store.resolve(emitter.range()));
      break;
    case JsonEmitter::State::Done:
      if (!emitter.status().ok()) {
        return emitter.status();
      }
      return out;
    }
  }
}

// The emitter's output must be identical however finely it is chopped, so every
// case is run at the default threshold and at one byte.
void expectEmits(const nlohmann::json& root, const FakeStore& store, absl::string_view expected) {
  const absl::StatusOr<std::string> whole = emit(root, store);
  ASSERT_THAT(whole.status(), IsOk());
  EXPECT_EQ(*whole, expected);

  const absl::StatusOr<std::string> chopped = emit(root, store, /*flush_bytes=*/1);
  ASSERT_THAT(chopped.status(), IsOk());
  EXPECT_EQ(*chopped, expected) << "output changed when flushed one byte at a time";
}

TEST(JsonEmitterTest, Scalars) {
  FakeStore store;
  expectEmits(nlohmann::json("hello"), store, "\"hello\"");
  expectEmits(nlohmann::json(true), store, "true");
  expectEmits(nlohmann::json(false), store, "false");
  expectEmits(nlohmann::json(nullptr), store, "null");
  expectEmits(nlohmann::json(42), store, "42");
  expectEmits(nlohmann::json(-7), store, "-7");
}

TEST(JsonEmitterTest, Doubles) {
  FakeStore store;
  expectEmits(nlohmann::json(0.5), store, "0.5");
  expectEmits(nlohmann::json(1.0), store, "1");
}

// JSON has no literal for a non-finite number; null is the conforming choice.
TEST(JsonEmitterTest, NonFiniteDoublesBecomeNull) {
  FakeStore store;
  expectEmits(nlohmann::json(std::nan("")), store, "null");
  expectEmits(nlohmann::json(std::numeric_limits<double>::infinity()), store, "null");
}

TEST(JsonEmitterTest, StringsAreEscaped) {
  FakeStore store;
  expectEmits(nlohmann::json("a\"b\\c\nd"), store, "\"a\\\"b\\\\c\\nd\"");
  // Valid UTF-8 passes through rather than being \u-escaped.
  expectEmits(nlohmann::json("caf\xc3\xa9"), store, "\"caf\xc3\xa9\"");
}

TEST(JsonEmitterTest, EmptyContainers) {
  FakeStore store;
  expectEmits(nlohmann::json::object(), store, "{}");
  expectEmits(nlohmann::json::array(), store, "[]");
}

TEST(JsonEmitterTest, NestedContainers) {
  FakeStore store;
  nlohmann::json root = nlohmann::json::object();
  root["a"] = nlohmann::json::array({1, 2, 3});
  root["b"] = nlohmann::json::object();
  root["b"]["c"] = "d";
  expectEmits(root, store, "{\"a\":[1,2,3],\"b\":{\"c\":\"d\"}}");
}

// Keys are escaped the same way values are.
TEST(JsonEmitterTest, KeysAreEscaped) {
  FakeStore store;
  nlohmann::json root = nlohmann::json::object();
  root["a\"b"] = 1;
  expectEmits(root, store, "{\"a\\\"b\":1}");
}

// The point of the emitter: an offloaded value is spliced in from the store,
// verbatim, between quotes the emitter supplies.
TEST(JsonEmitterTest, SplicesOffloadedValue) {
  FakeStore store{"XXXXhello worldYYY"};
  nlohmann::json root = nlohmann::json::object();
  root["content"] = JsonWithExtBuf::makeExternalRef({/*offset=*/4, /*length=*/11});
  expectEmits(root, store, "{\"content\":\"hello world\"}");
}

// The stored bytes are raw JSON content, already escaped. They must be spliced
// as-is: escaping them again would double every backslash.
TEST(JsonEmitterTest, SplicedBytesAreNotReEscaped) {
  FakeStore store{"line\\nbreak \\\"quoted\\\""};
  nlohmann::json root = nlohmann::json::object();
  root["content"] = JsonWithExtBuf::makeExternalRef({/*offset=*/0, /*length=*/store.bytes.size()});
  expectEmits(root, store, "{\"content\":\"line\\nbreak \\\"quoted\\\"\"}");
}

TEST(JsonEmitterTest, MultipleOffloadedValues) {
  FakeStore store{"firstsecond"};
  nlohmann::json root = nlohmann::json::object();
  root["a"] = JsonWithExtBuf::makeExternalRef({0, 5});
  root["b"] = JsonWithExtBuf::makeExternalRef({5, 6});
  expectEmits(root, store, "{\"a\":\"first\",\"b\":\"second\"}");
}

TEST(JsonEmitterTest, OffloadedValuesInsideArrays) {
  FakeStore store{"onetwo"};
  nlohmann::json root = nlohmann::json::array();
  root.push_back(JsonWithExtBuf::makeExternalRef({0, 3}));
  root.push_back("plain");
  root.push_back(JsonWithExtBuf::makeExternalRef({3, 3}));
  expectEmits(root, store, "[\"one\",\"plain\",\"two\"]");
}

// A zero-length range is a legitimately empty offloaded string.
TEST(JsonEmitterTest, EmptyOffloadedValue) {
  FakeStore store{"abc"};
  nlohmann::json root = nlohmann::json::object();
  root["content"] = JsonWithExtBuf::makeExternalRef({1, 0});
  expectEmits(root, store, "{\"content\":\"\"}");
}

// A realistic shape: the OpenAI request the filter parses, with the prompt
// offloaded and everything else inline.
TEST(JsonEmitterTest, ChatCompletionsShape) {
  FakeStore store{"You are a helpful assistant."};
  nlohmann::json message = nlohmann::json::object();
  message["role"] = "user";
  message["content"] = JsonWithExtBuf::makeExternalRef({0, store.bytes.size()});

  nlohmann::json root = nlohmann::json::object();
  root["messages"] = nlohmann::json::array({message});
  root["model"] = "gpt-4";
  root["temperature"] = 0.5;

  expectEmits(root, store,
              "{\"messages\":[{\"content\":\"You are a helpful assistant.\",\"role\":\"user\"}],"
              "\"model\":\"gpt-4\",\"temperature\":0.5}");
}

// A payload big enough to span many flushes still reassembles exactly.
TEST(JsonEmitterTest, LargeDocumentAcrossManyFlushes) {
  FakeStore store;
  nlohmann::json root = nlohmann::json::array();
  std::string expected = "[";
  for (int i = 0; i < 200; ++i) {
    root.push_back(std::string(100, 'a' + (i % 26)));
    if (i > 0) {
      expected += ",";
    }
    expected += "\"" + std::string(100, 'a' + (i % 26)) + "\"";
  }
  expected += "]";

  const absl::StatusOr<std::string> out = emit(root, store, /*flush_bytes=*/64);
  ASSERT_THAT(out.status(), IsOk());
  EXPECT_EQ(*out, expected);
}

// Text is handed back in bounded runs rather than accumulated whole.
TEST(JsonEmitterTest, FlushThresholdBoundsEachRun) {
  FakeStore store;
  nlohmann::json root = nlohmann::json::array();
  for (int i = 0; i < 50; ++i) {
    root.push_back(std::string(64, 'x'));
  }

  JsonEmitter emitter(root, /*flush_bytes=*/128);
  size_t runs = 0;
  size_t longest = 0;
  while (true) {
    const JsonEmitter::State state = emitter.next();
    if (state == JsonEmitter::State::Done) {
      break;
    }
    ASSERT_EQ(state, JsonEmitter::State::Text);
    ++runs;
    longest = std::max(longest, emitter.text().size());
  }
  EXPECT_GT(runs, 1);
  // A run is cut once it reaches the threshold, so it can overshoot by at most
  // the single value that crossed it.
  EXPECT_LT(longest, 128 + 128);
}

// Done is terminal: calling next() again keeps reporting Done rather than
// restarting or walking off the stack.
TEST(JsonEmitterTest, DoneIsIdempotent) {
  FakeStore store;
  JsonEmitter emitter(nlohmann::json(1));
  ASSERT_EQ(emitter.next(), JsonEmitter::State::Text);
  ASSERT_EQ(emitter.next(), JsonEmitter::State::Done);
  EXPECT_EQ(emitter.next(), JsonEmitter::State::Done);
  EXPECT_THAT(emitter.status(), IsOk());
}

// A binary node that is not an offload reference has no JSON rendering. The
// parser never produces one, so this reports rather than guesses.
TEST(JsonEmitterTest, RejectsForeignBinaryNode) {
  nlohmann::json root = nlohmann::json::object();
  root["x"] = nlohmann::json::binary({1, 2, 3});

  JsonEmitter emitter(root);
  while (emitter.next() != JsonEmitter::State::Done) {
  }
  EXPECT_THAT(emitter.status(), StatusCodeIs(absl::StatusCode::kInternal));
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
