#pragma once

#include <string>

#include "source/common/json/wuffs_json/wuffs_json_cursor.h"

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Decodes the raw bytes of an offloaded JSON string back into the text it
// denotes.
//
// An offloaded value is stored as the JSON source between the quotes -- still
// escaped, quotes excluded (json_with_ext_buf.h). Those bytes are what a filter
// would see if it read an ExternalRef range straight out of the buffer, and they
// are the wrong thing to hand it: a filter matching on prompt content should see
// a newline, not the two characters `\` and `n`, and should never have to worry
// about a chunk boundary splitting an escape sequence.
//
// This is the inverse of Json::sanitize() (source/common/json/json_sanitizer.h),
// which produces exactly this representation from text. The two round-trip, which
// is what lets a modified field be re-escaped and written back.
//
// Decoding is delegated to the same Wuffs cursor the payload parser uses, by
// feeding it a synthetic one-string document: the quotes are supplied here and
// the caller's bytes go between them. That reuses the cursor's escape handling
// (including escapes split across chunk boundaries) rather than repeating it.
//
// Usage:
//
//   JsonStringDecoder decoder([](absl::string_view text) { ... });
//   for (chunk : range_chunks) {
//     RETURN_IF_NOT_OK(decoder.feed(chunk, is_last));
//   }
class JsonStringDecoder : public Json::Wuffs::WuffsJsonCursor::Handler {
public:
  // Receives each decoded UTF-8 run, in order. The view is valid only for the
  // duration of the call. A single feed() may produce several runs, or none.
  using DecodedCallback = absl::AnyInvocable<void(absl::string_view)>;

  explicit JsonStringDecoder(DecodedCallback on_decoded);

  // Feeds one chunk of raw escaped content. Chunks may split at any byte,
  // including inside an escape sequence. `end` must be true on the final chunk
  // and only then; it is what closes the synthetic document, so a trailing
  // backslash or a truncated \u escape is reported there.
  //
  // Errors are terminal and sticky: once a feed fails, every later feed returns
  // the same error rather than decoding on from an unknown state.
  absl::Status feed(absl::string_view chunk, bool end);

  // Json::Wuffs::WuffsJsonCursor::Handler
  bool openStringCapture(absl::string_view key, int depth, size_t token_start) override;
  bool onStringChunk(absl::string_view key, int depth, absl::string_view chunk) override;
  void closeStringCapture(absl::string_view key, int depth, size_t token_end) override;
  absl::Status onKey(absl::string_view key, int depth, size_t token_start) override;
  absl::Status onNumber(absl::string_view key, absl::string_view raw, int depth, size_t token_start,
                        size_t token_end) override;
  absl::Status onBoolean(absl::string_view key, bool value, int depth, size_t token_start,
                         size_t token_end) override;
  void onNull(absl::string_view key, int depth, size_t token_start, size_t token_end) override;
  void onContainerOpen(absl::string_view key, bool is_dict, int depth, size_t token_start) override;
  void onContainerClose(int depth, size_t token_end) override;

private:
  // Records the first error and latches it. Later feeds replay it.
  void setError(absl::Status status);

  // Every callback other than the string ones means the bytes did not denote a
  // plain string -- only reachable from a corrupted range, since a well-formed
  // one cannot close the synthetic string and open another value. Fails rather
  // than silently decoding a prefix.
  void notAString();

  DecodedCallback on_decoded_;
  Json::Wuffs::WuffsJsonCursor cursor_;

  // False until the synthetic opening quote has been fed.
  bool opened_{false};
  // True once the final chunk has been accepted; a further feed is a caller bug.
  bool finished_{false};
  absl::Status status_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
