#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"

// Wuffs JSON tokenizer — declarations only.
// WUFFS_IMPLEMENTATION is defined in exactly one translation unit: wuffs_impl.c.
#include "release/c/wuffs-v0.4.c"

namespace Envoy {
namespace Json {

// WuffsJsonCursor — streaming SAX-style JSON parser built on the Wuffs library
// (https://github.com/google/wuffs).
//
// The cursor tokenizes a JSON document delivered as a sequence of byte chunks
// (e.g. HTTP body fragments arriving on an event loop) and fires synchronous
// callbacks into a Handler as semantic events are recognized.  All low-level
// Wuffs mechanics — token ring-buffer management, coroutine suspension and
// resumption, escape sequence decoding — are hidden behind the Handler
// interface; consumers see only clean, decoded events.
//
// ── Streaming model ──────────────────────────────────────────────────────────
//
// Call feed(chunk, closed) for each incoming chunk; set closed=true on the
// final one to signal EOF.  The Wuffs decoder is a class member that preserves
// all parse state across calls, so chunks may split at any byte boundary —
// including the middle of a string value or a multi-digit number:
//
//   class MyHandler : public WuffsJsonCursor::Handler { ... };
//   MyHandler h;
//   WuffsJsonCursor cursor(h);
//   for (const Chunk& c : http_body_chunks) {
//     if (auto s = cursor.feed(c.data, c.is_last); !s.ok()) { /* error */ }
//   }
//
// ── Depth and key model ───────────────────────────────────────────────────────
//
// Every callback receives `depth` — the nesting level of the value or container
// being reported.  Depth starts at 0 before the root container.
// onContainerOpen fires after depth is incremented; onContainerClose fires
// before it is decremented, so both report the depth of the container itself.
//
//   { "a": { "b": [ 1, 2 ] } }
//    ^d=1    ^d=2   ^d=3
//
// For values inside a dict, callbacks also receive `key` — the dict key
// immediately to the left of the value.  For array elements, key is "".
// The cursor tracks the current key internally, so handlers never need a
// separate current_key_ member.
//
// Example callback sequence for {"messages": [{"role": "user"}]}:
//
//   onContainerOpen (key="",         is_dict=true,  depth=1)  ← root {
//   onKey           ("messages",                    depth=1)
//   onContainerOpen (key="messages", is_dict=false, depth=2)  ← [
//   onContainerOpen (key="",         is_dict=true,  depth=3)  ← { (parent is array)
//   onKey           ("role",                        depth=3)
//   selectStringTarget("role",                      depth=3)  → &buf or nullptr
//   onStringComplete  (&buf,                        depth=3)
//   onContainerClose (depth=3, ...)
//   onContainerClose (depth=2, ...)
//   onContainerClose (depth=1, ...)
//
// ── String value lifecycle ────────────────────────────────────────────────────
//
// A JSON string value may span multiple feed() calls if a chunk boundary falls
// inside the string.  The sequence is always:
//   1. selectStringTarget(key, depth)  — handler returns &buf to collect into,
//                                        or nullptr to discard at zero cost.
//   2. (cursor appends decoded UTF-8 bytes to buf as token segments arrive)
//   3. onStringComplete(&buf, depth)   — fired when the closing quote is seen.
//
// Backslash escapes (\n, \t, \uXXXX, …) are decoded transparently — the
// handler always receives valid UTF-8, never raw escape sequences.
//
// ── Container byte ranges ─────────────────────────────────────────────────────
//
// onContainerOpen receives tok_start — the byte offset of the opening { or [
// in the original body stream.  onContainerClose receives tok_end — the offset
// immediately past the closing } or ].  Together they delimit a half-open byte
// range [tok_start, tok_end) suitable for zero-copy sub-range extraction when
// the body is memory-mapped or stored contiguously.
//
// ── Path tracking ─────────────────────────────────────────────────────────────
//
// Construct with track_paths=true and call buildPaths(depth, indexed, pattern)
// from within any callback to obtain dot-notation path strings for the current
// position:
//   indexed_path  "messages[0].role"   — concrete index, for per-element keys
//   pattern_path  "messages[].role"    — wildcard index, for config matching
//
class WuffsJsonCursor {
public:
  enum class ScalarKind { kNumber, kLiteral };

  // Handler — callback interface implemented by the JSON document consumer.
  //
  // All callbacks are invoked synchronously from within feed().
  //
  //   selectStringTarget(key, depth)
  //     Called at the start of every non-key string value chain.
  //     `key` is the dict key for this value, or "" for array elements.
  //     Return a std::string* to accumulate decoded characters into, or nullptr
  //     to discard the value without any heap allocation.
  //
  //   onKey(key, depth)
  //     Called when a key string chain completes.  Return a non-OK Status to
  //     abort parsing (e.g. duplicate-key detection).
  //
  //   onStringComplete(target, depth)
  //     Called when a non-key string chain completes.  `target` is the same
  //     pointer returned by selectStringTarget(); it is never null here.
  //
  //   onScalar(key, raw, kind, depth)
  //     Called for NUMBER and LITERAL (true/false/null) tokens.
  //     `key` is the dict key for this value, or "" for array elements.
  //     `raw` is the source bytes; `kind` distinguishes NUMBER from LITERAL.
  //     Return a non-OK Status to abort parsing.
  //
  //   onContainerOpen(key, is_dict, depth, tok_start)
  //     Called after depth has been incremented for a { or [ open.
  //     `key` is the parent dict key that opened this container, or "" if the
  //     parent is an array or this is the root container.
  //     `tok_start` is the byte offset of the opening delimiter in the original
  //     body stream, useful for byte-range recording.
  //
  //   onContainerClose(depth, tok_end)
  //     Called with the container's depth before decrement and the byte offset
  //     immediately after the closing } or ].
  class Handler {
  public:
    virtual ~Handler() = default;
    virtual std::string* selectStringTarget(absl::string_view key, int depth) = 0;
    virtual absl::Status onKey(absl::string_view key, int depth) = 0;
    virtual void onStringComplete(std::string* target, int depth) = 0;
    virtual absl::Status onScalar(absl::string_view key, absl::string_view raw,
                                  ScalarKind kind, int depth) = 0;
    virtual void onContainerOpen(absl::string_view key, bool is_dict, int depth,
                                 size_t tok_start) = 0;
    virtual void onContainerClose(int depth, size_t tok_end) = 0;
  };

  explicit WuffsJsonCursor(Handler& handler, bool track_paths = false);

  // Feed one body chunk.  Set closed=true on the final chunk (signals EOF to Wuffs).
  // Returns non-OK on malformed JSON or internal allocation failure.
  absl::Status feed(absl::string_view chunk, bool closed);

  // Build dot-notation path strings for the field currently being selected.
  // indexed_path: e.g. "messages[0].role"   pattern_path: e.g. "messages[].role"
  // Must only be called from within a Handler callback while feed() is active.
  void buildPaths(int depth, std::string& indexed_path, std::string& pattern_path) const;

  // Monotonically increasing byte offset of the next source byte to be consumed.
  // Matches the tok_start / tok_end values delivered to onContainerOpen / onContainerClose.
  size_t srcPos() const { return body_src_pos_; }

private:
  Handler& handler_;
  bool     track_paths_;

  wuffs_json__decoder::unique_ptr dec_;
  static constexpr size_t  kTokBufLen = 256;
  wuffs_base__token        tok_data_[kTokBufLen];
  wuffs_base__token_buffer tok_buf_{};

  size_t body_src_pos_{0};
  bool   wuffs_done_{false};

  static constexpr int kMaxDepth = 8;
  int  depth_{0};
  bool is_dict_[kMaxDepth]{};
  bool expecting_key_[kMaxDepth]{};

  // key_stack_[d]   — most recently completed key at dict depth d.
  //                   Always maintained (not gated on track_paths_) because it
  //                   is forwarded as the `key` argument to Handler callbacks.
  // push_key_[d]    — key at depth d-1 that opened the container at depth d;
  //                   captured at push time.  Size kMaxDepth+1 so push_key_[kMaxDepth]
  //                   is accessible when depth_ reaches kMaxDepth.
  //                   Only maintained when track_paths_=true (used by buildPaths).
  // array_index_[d] — count of elements already completed at array depth d;
  //                   reset to 0 on container open, incremented on container close
  //                   when the parent container is an array.
  //                   Only maintained when track_paths_=true (used by buildPaths).
  std::string key_stack_[kMaxDepth]{};
  std::string push_key_[kMaxDepth + 1]{};
  int         array_index_[kMaxDepth]{};

  bool         in_chain_{false};
  bool         string_is_key_{false};
  std::string  str_acc_;
  std::string* str_target_{nullptr};
};

} // namespace Json
} // namespace Envoy
