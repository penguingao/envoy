#include <string>
#include <vector>

#include "source/common/json/json_sanitizer.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_string_decoder.h"

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

// Decodes `raw` fed as a single chunk.
absl::StatusOr<std::string> decodeWhole(absl::string_view raw) {
  std::string out;
  JsonStringDecoder decoder(
      [&out](absl::string_view text) { out.append(text.data(), text.size()); });
  if (absl::Status status = decoder.feed(raw, /*end=*/true); !status.ok()) {
    return status;
  }
  return out;
}

// Decodes `raw` one byte at a time, so every escape sequence is split at every
// interior boundary. Any state the decoder fails to carry across a feed shows up
// as a mismatch with decodeWhole().
absl::StatusOr<std::string> decodeByteByByte(absl::string_view raw) {
  std::string out;
  JsonStringDecoder decoder(
      [&out](absl::string_view text) { out.append(text.data(), text.size()); });
  if (raw.empty()) {
    if (absl::Status status = decoder.feed("", /*end=*/true); !status.ok()) {
      return status;
    }
    return out;
  }
  for (size_t i = 0; i < raw.size(); ++i) {
    if (absl::Status status = decoder.feed(raw.substr(i, 1), /*end=*/i + 1 == raw.size());
        !status.ok()) {
      return status;
    }
  }
  return out;
}

// Decoding must not depend on how the input is chunked.
void expectDecodes(absl::string_view raw, absl::string_view expected) {
  const absl::StatusOr<std::string> whole = decodeWhole(raw);
  ASSERT_THAT(whole.status(), IsOk());
  EXPECT_EQ(*whole, expected) << "whole-input decode of [" << raw << "]";

  const absl::StatusOr<std::string> split = decodeByteByByte(raw);
  ASSERT_THAT(split.status(), IsOk());
  EXPECT_EQ(*split, expected) << "byte-by-byte decode of [" << raw << "]";
}

TEST(JsonStringDecoderTest, PlainText) { expectDecodes("hello world", "hello world"); }

TEST(JsonStringDecoderTest, Empty) { expectDecodes("", ""); }

TEST(JsonStringDecoderTest, ShortEscapes) {
  expectDecodes("a\\nb", "a\nb");
  expectDecodes("a\\tb", "a\tb");
  expectDecodes("a\\rb", "a\rb");
  expectDecodes("a\\bb", "a\bb");
  expectDecodes("a\\fb", "a\fb");
  expectDecodes("a\\\"b", "a\"b");
  expectDecodes("a\\\\b", "a\\b");
  expectDecodes("a\\/b", "a/b");
}

TEST(JsonStringDecoderTest, UnicodeEscapes) {
  expectDecodes("caf\\u00e9", "caf\xc3\xa9");
  expectDecodes("\\u0041", "A");
  // Control characters are only expressible as \u escapes.
  expectDecodes("a\\u0000b", std::string("a\0b", 3));
}

// A surrogate pair spans two \u escapes; splitting between them must still
// produce the single code point.
TEST(JsonStringDecoderTest, SurrogatePair) {
  expectDecodes("\\ud83d\\ude00", "\xf0\x9f\x98\x80"); // U+1F600
}

TEST(JsonStringDecoderTest, RawUtf8PassesThrough) {
  expectDecodes("caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac");
}

// The realistic shape: a long prompt with embedded newlines, which is what
// offloading produces.
TEST(JsonStringDecoderTest, LongValueWithEscapes) {
  const std::string raw = std::string(4096, 'x') + "\\n" + std::string(4096, 'y');
  const std::string expected = std::string(4096, 'x') + "\n" + std::string(4096, 'y');
  expectDecodes(raw, expected);
}

// Round-trip with Json::sanitize(), which this is the inverse of. That pairing
// is what makes "decode, let a filter edit, re-escape, write back" sound.
TEST(JsonStringDecoderTest, RoundTripsWithJsonSanitize) {
  const std::vector<std::string> values = {
      "plain",
      "",
      "with \"quotes\"",
      "with \\ backslash",
      "line\nbreak\ttab",
      "caf\xc3\xa9",
      "\xf0\x9f\x98\x80",
      std::string("nul\0inside", 10),
      "mixed \"q\" \\ and \xe6\x97\xa5 and \x01 control",
  };

  for (const std::string& value : values) {
    std::string buffer;
    const absl::string_view escaped = Json::sanitize(buffer, value);
    const absl::StatusOr<std::string> decoded = decodeWhole(escaped);
    ASSERT_THAT(decoded.status(), IsOk())
        << "value [" << value << "] escaped as [" << escaped << "]";
    EXPECT_EQ(*decoded, value) << "escaped form was [" << escaped << "]";
  }
}

// A range cut short mid-escape is rejected at end rather than silently dropping
// the tail: the closing of the document is what catches it.
TEST(JsonStringDecoderTest, RejectsTrailingBackslash) {
  EXPECT_THAT(decodeWhole("abc\\").status(), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

TEST(JsonStringDecoderTest, RejectsTruncatedUnicodeEscape) {
  EXPECT_THAT(decodeWhole("abc\\u00").status(), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

TEST(JsonStringDecoderTest, RejectsInvalidEscape) {
  EXPECT_THAT(decodeWhole("a\\xb").status(), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

// A raw control character is not legal inside a JSON string; a range holding one
// did not come from a parsed payload.
TEST(JsonStringDecoderTest, RejectsRawControlCharacter) {
  EXPECT_THAT(decodeWhole(std::string("a\nb")).status(),
              StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

// A range carrying an unescaped quote would close the synthetic string early.
// Whatever follows cannot form a plain string, so it is rejected rather than
// decoded as a prefix.
TEST(JsonStringDecoderTest, RejectsUnescapedQuote) {
  EXPECT_FALSE(decodeWhole("a\",\"b").status().ok());
  EXPECT_FALSE(decodeWhole("a\":1,\"b").status().ok());
}

// A range that closes the string and opens a container hits the non-string
// callbacks rather than a cursor syntax error.
TEST(JsonStringDecoderTest, RejectsNonStringDocument) {
  EXPECT_THAT(decodeWhole("\",[\"x").status(), StatusCodeIs(absl::StatusCode::kInvalidArgument));
}

// Errors are terminal: a decoder that failed keeps reporting the same failure
// rather than resuming from an unknown state.
TEST(JsonStringDecoderTest, ErrorIsSticky) {
  std::string out;
  JsonStringDecoder decoder(
      [&out](absl::string_view text) { out.append(text.data(), text.size()); });

  const absl::Status first = decoder.feed("a\nb", /*end=*/false);
  ASSERT_FALSE(first.ok());
  const absl::Status second = decoder.feed("more", /*end=*/false);
  EXPECT_EQ(second.message(), first.message());
  const absl::Status third = decoder.feed("", /*end=*/true);
  EXPECT_EQ(third.message(), first.message());
}

// Feeding past the end is a caller bug, reported rather than decoded.
TEST(JsonStringDecoderTest, FeedAfterEndFails) {
  std::string out;
  JsonStringDecoder decoder(
      [&out](absl::string_view text) { out.append(text.data(), text.size()); });

  ASSERT_THAT(decoder.feed("abc", /*end=*/true), IsOk());
  EXPECT_THAT(decoder.feed("more", /*end=*/true),
              StatusCodeIs(absl::StatusCode::kFailedPrecondition));
}

// Content may arrive as several chunks with the terminator on an empty one,
// which is how a range read that lands on a chunk boundary finishes.
TEST(JsonStringDecoderTest, EmptyFinalChunk) {
  std::string out;
  JsonStringDecoder decoder(
      [&out](absl::string_view text) { out.append(text.data(), text.size()); });

  ASSERT_THAT(decoder.feed("ab", /*end=*/false), IsOk());
  ASSERT_THAT(decoder.feed("cd", /*end=*/false), IsOk());
  ASSERT_THAT(decoder.feed("", /*end=*/true), IsOk());
  EXPECT_EQ(out, "abcd");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
