#include "source/common/json/wuffs_json/wuffs_json.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/numbers.h"

namespace Envoy {
namespace Json {
namespace {

// ── Capturing handler ─────────────────────────────────────────────────────────
// Records depth-1 fields from a JSON object. Deeper content is discarded.
// Scalars are stored as raw strings; the test asserts on them directly.

struct CapturingHandler : WuffsJsonCursor::Handler {
  struct Field {
    std::string key;
    std::string str_val;  // for string fields
    std::string raw_val;  // for scalars (NUMBER / LITERAL raw bytes)
    bool is_string{false};
    bool is_scalar{false};
  };

  std::vector<Field> fields;
  std::string pending_key_;
  std::string pending_str_;

  std::string* selectStringTarget(absl::string_view /*key*/, int depth) override {
    return (depth == 1) ? &pending_str_ : nullptr;
  }

  absl::Status onKey(absl::string_view key, int depth) override {
    if (depth == 1) pending_key_ = std::string(key);
    return absl::OkStatus();
  }

  void onStringComplete(std::string* /*target*/, int depth) override {
    if (depth == 1) {
      fields.push_back({pending_key_, pending_str_, {}, /*is_string=*/true});
      pending_str_.clear();
    }
  }

  absl::Status onScalar(absl::string_view /*key*/, absl::string_view raw,
                        WuffsJsonCursor::ScalarKind /*kind*/, int depth) override {
    if (depth == 1)
      fields.push_back({pending_key_, {}, std::string(raw), {}, /*is_scalar=*/true});
    return absl::OkStatus();
  }

  void onContainerOpen(absl::string_view /*key*/, bool /*is_dict*/, int /*depth*/,
                       size_t /*tok_start*/) override {}
  void onContainerClose(int /*depth*/, size_t /*tok_end*/) override {}
};

// Helper: parse a complete JSON string in one shot.
absl::Status parse(absl::string_view json, CapturingHandler& h) {
  WuffsJsonCursor cursor(h);
  return cursor.feed(json, /*closed=*/true);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(WuffsJsonCursorTest, EmptyObject) {
  CapturingHandler h;
  EXPECT_TRUE(parse("{}", h).ok());
  EXPECT_TRUE(h.fields.empty());
}

TEST(WuffsJsonCursorTest, FlatStringFields) {
  CapturingHandler h;
  EXPECT_TRUE(parse(R"({"model":"gpt-4","role":"user"})", h).ok());
  ASSERT_EQ(h.fields.size(), 2u);
  EXPECT_EQ(h.fields[0].key, "model");
  EXPECT_EQ(h.fields[0].str_val, "gpt-4");
  EXPECT_EQ(h.fields[1].key, "role");
  EXPECT_EQ(h.fields[1].str_val, "user");
}

TEST(WuffsJsonCursorTest, ScalarFields) {
  CapturingHandler h;
  EXPECT_TRUE(parse(R"({"count":42,"ratio":1.5,"ok":true,"x":null})", h).ok());
  ASSERT_EQ(h.fields.size(), 4u);
  EXPECT_EQ(h.fields[0].raw_val, "42");
  EXPECT_EQ(h.fields[1].raw_val, "1.5");
  EXPECT_EQ(h.fields[2].raw_val, "true");
  EXPECT_EQ(h.fields[3].raw_val, "null");
}

// Wuffs emits \n, \t, and \uXXXX as UNICODE_CODE_POINT tokens (VBC=3),
// not STRING tokens.  This test verifies the cursor handles them correctly.
TEST(WuffsJsonCursorTest, StringEscapes) {
  CapturingHandler h;
  EXPECT_TRUE(parse(R"({"nl":"hello\nworld","tab":"a\tb","uni":"A"})", h).ok());
  ASSERT_EQ(h.fields.size(), 3u);
  EXPECT_EQ(h.fields[0].str_val, "hello\nworld");
  EXPECT_EQ(h.fields[1].str_val, "a\tb");
  EXPECT_EQ(h.fields[2].str_val, "A"); // U+0041
}

// Deeper-than-1 content is discarded (selectStringTarget returns nullptr).
TEST(WuffsJsonCursorTest, NestedObjectDiscarded) {
  CapturingHandler h;
  EXPECT_TRUE(parse(R"({"top":"v","nested":{"a":"b"}})", h).ok());
  // "top" is a depth-1 string → captured.
  // "nested" value is a depth-1 push → onPush fired, but inner "a"/"b" at
  // depth 2 have selectStringTarget return nullptr, so they are discarded.
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].str_val, "v");
}

// Feed the document in two chunks to verify the Wuffs decoder state persists.
TEST(WuffsJsonCursorTest, StreamingAcrossChunks) {
  CapturingHandler h;
  WuffsJsonCursor cursor(h);

  // The string value "gpt-4" straddles the chunk boundary.
  EXPECT_TRUE(cursor.feed(R"({"model":"gpt)", /*closed=*/false).ok());
  EXPECT_TRUE(cursor.feed(R"(-4","n":7})", /*closed=*/true).ok());

  ASSERT_EQ(h.fields.size(), 2u);
  EXPECT_EQ(h.fields[0].str_val, "gpt-4");
  EXPECT_EQ(h.fields[1].raw_val, "7");
}

TEST(WuffsJsonCursorTest, InvalidJsonReturnsError) {
  CapturingHandler h;
  EXPECT_FALSE(parse("not json", h).ok());
}

} // namespace
} // namespace Json
} // namespace Envoy
