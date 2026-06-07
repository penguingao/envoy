#include "source/common/json/wuffs_json/wuffs_json.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Json {

namespace {

// Append the content of one Wuffs STRING token to `out`.
// JSON STRING tokens carry either plain bytes (COPY) or the opening/closing
// quote delimiter (DROP).  Backslash escapes do NOT arrive as STRING tokens —
// Wuffs emits them as UNICODE_CODE_POINT tokens handled separately below.
void appendStringToken(std::string& out, absl::string_view raw, uint64_t vbd) {
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) return;
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_1_DST_1_SRC_COPY) {
    out.append(raw.data(), raw.size());
  }
}

// UTF-8 encode a Unicode code point and append it to `out`.
void appendCodePoint(std::string& out, uint32_t cp) {
  if (cp < 0x80u) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800u) {
    out += static_cast<char>(0xC0u | (cp >> 6u));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else if (cp < 0x10000u) {
    out += static_cast<char>(0xE0u | (cp >> 12u));
    out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else {
    out += static_cast<char>(0xF0u | (cp >> 18u));
    out += static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu));
    out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  }
}

} // namespace

WuffsJsonCursor::WuffsJsonCursor(Handler& handler, bool track_paths)
    : handler_(handler), track_paths_(track_paths) {
  dec_     = wuffs_json__decoder::alloc();
  tok_buf_ = wuffs_base__slice_token__writer(
      wuffs_base__make_slice_token(tok_data_, kTokBufLen));
}

// Feeds one body chunk into the Wuffs JSON tokenizer and dispatches tokens
// to the Handler callbacks.
//
// Wuffs token model
// ─────────────────
// Each wuffs_base__token carries three fields used here:
//
//   vbc  (value_base_category) — coarse token kind; the switch below:
//     FILLER     Whitespace and punctuation (commas, colons, quotes).
//                No semantic meaning; silently skipped.
//     STRUCTURE  Container open/close: { } [ ].
//                vbd bits distinguish open vs. close and dict vs. array:
//                  VBD__STRUCTURE__PUSH    set on { or [
//                  VBD__STRUCTURE__TO_DICT set on { (clear on [)
//                depth_ is incremented before onContainerOpen and decremented
//                after onContainerClose, so the handler always sees the depth
//                of the container itself.
//     STRING     One segment of a JSON string value or key — plain bytes only.
//                A single logical string may span multiple tokens if Wuffs fills
//                tok_buf_ before the closing quote; the `cont` flag
//                (token__continued) is true on all but the last segment.
//                On the first token of a new value string (!in_chain_),
//                openStringCapture is called — the handler inspects key+depth and
//                returns a handler-owned buffer pointer (stored as str_target_), or
//                nullptr to discard.  Every subsequent STRING token for the same
//                string appends to *str_target_ if non-null, or skips if nullptr —
//                zero cost regardless of string length.  On the last token
//                (cont=false), closeStringCapture fires with the same str_target_.
//                Key strings bypass openStringCapture: they accumulate into the
//                internal str_acc_ buffer and fire onKey on completion.
//                vbd bits used by appendStringToken:
//                  VBD__STRING__CONVERT_0_DST_1_SRC_DROP  — opening/closing quote
//                  VBD__STRING__CONVERT_1_DST_1_SRC_COPY  — plain ASCII bytes
//                NOTE: backslash escapes do NOT arrive as STRING tokens — Wuffs
//                emits them as UNICODE_CODE_POINT tokens (see below).
//     UNICODE_CODE_POINT
//                A decoded escape sequence (\n, \t, \uXXXX, …).  vbd holds the
//                Unicode code point value directly.  The token's length covers
//                the raw escape bytes in the source (e.g. 2 bytes for "\n").
//                Decoded to UTF-8 and appended to *str_target_ (same pointer
//                openStringCapture returned); skipped silently if str_target_ is
//                nullptr.  in_chain_ is NOT updated here — its lifecycle is
//                managed by the surrounding STRING tokens.
//     NUMBER     A JSON number literal (integer or floating-point).
//                Raw bytes forwarded to onNumber(key, raw, depth).
//     LITERAL    One of: true, false, null.
//                Dispatched to onBoolean(key, value, depth) or onNull(key, depth).
//
//   vbd  (value_base_detail) — kind-specific bit flags (see vbc above).
//
//   tlen (token__length) — number of source bytes this token consumed.
//        body_src_pos_ is advanced by tlen for every token regardless of vbc,
//        giving a monotonically increasing byte counter.  onContainerOpen and
//        onContainerClose deliver tok_start / tok_end from this counter.
//
// VBC dispatch summary
// ────────────────────
//   VBC constant        Meaning                           Action
//   ─────────────────   ──────────────────────────────   ──────────────────────────────────────────
//   FILLER              Whitespace, commas, colons        Advance body_src_pos_; no other action
//   STRUCTURE           Object/array open or close        Manage depth_, is_dict_[], expecting_key_[];
//                                                         record byte ranges for containers
//   STRING              String content or quote delim     Gate on str_target_; DROP tokens skip,
//                       (plain bytes only — no escapes)   COPY tokens append via appendStringToken
//   UNICODE_CODE_POINT  Decoded backslash escape          Gate on str_target_; decode code point to
//                       (\n, \t, \uXXXX, …)              UTF-8 and append via appendCodePoint
//   NUMBER              Numeric literal                   Forward raw bytes to onNumber
//   LITERAL             true / false / null               Dispatch to onBoolean or onNull
//
// Outer loop suspension
// ─────────────────────
// decode_tokens returns one of three status classes:
//   nullptr / note  Complete — document fully consumed; set wuffs_done_.
//   short_read      tok_buf_ drained before src_buf exhausted — need more
//                   input; break and return, resuming on next feed() call.
//                   The Wuffs stackless coroutine in dec_ preserves all parse
//                   state so resumption is exact.
//   short_write     src_buf has input but tok_buf_ is full — reset the token
//                   ring buffer and re-invoke decode_tokens to drain more.
//   other error     Malformed JSON; propagate as InvalidArgumentError.
absl::Status WuffsJsonCursor::feed(absl::string_view chunk, bool closed) {
  if (!dec_) return absl::InternalError("wuffs json: alloc failed");
  if (wuffs_done_) return absl::OkStatus();

  // body_src_pos_ is a global byte counter across all feed() calls.  Token
  // offsets are expressed in the same global space, so (tok_start - chunk_base)
  // gives the offset into the current chunk — needed for chunk.substr() when
  // extracting raw bytes for STRING / NUMBER / LITERAL tokens.
  const size_t chunk_base = body_src_pos_;
  wuffs_base__io_buffer src_buf = wuffs_base__ptr_u8__reader(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(chunk.data())),
      chunk.size(), closed);

  while (true) {
    wuffs_base__status status = wuffs_json__decoder__decode_tokens(
        dec_.get(), &tok_buf_, &src_buf, wuffs_base__empty_slice_u8());

    while (tok_buf_.meta.ri < tok_buf_.meta.wi) {
      const wuffs_base__token* tok = &tok_buf_.data.ptr[tok_buf_.meta.ri++];
      const int64_t  vbc       = wuffs_base__token__value_base_category(tok);
      const uint64_t vbd       = wuffs_base__token__value_base_detail(tok);
      const uint64_t tlen      = wuffs_base__token__length(tok);
      const bool     cont      = wuffs_base__token__continued(tok);
      const size_t   tok_start = body_src_pos_;
      body_src_pos_ += tlen;

      switch (vbc) {

      // Whitespace, commas, colons, quote delimiters — no semantic value.
      case WUFFS_BASE__TOKEN__VBC__FILLER: break;

      // { } [ ] — container open (push) or close (pop).
      case WUFFS_BASE__TOKEN__VBC__STRUCTURE: {
        const bool is_push = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__PUSH) != 0;
        const bool to_dict = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__TO_DICT) != 0;
        if (is_push) {
          ++depth_;
          if (depth_ < kMaxDepth) {
            seen_keys_[depth_].clear();
            is_dict_[depth_]       = to_dict;
            expecting_key_[depth_] = to_dict;
            if (!to_dict) array_index_[depth_] = 0;
          }
          if (track_paths_ && depth_ <= kMaxDepth) {
            push_key_[depth_] = (depth_ > 1 && depth_ - 1 < kMaxDepth)
                                    ? key_stack_[depth_ - 1] : "";
          }
          // key for onContainerOpen is the parent dict key that triggered this
          // container.  Empty when the parent is an array or at root (depth 1).
          const absl::string_view push_key =
              (depth_ > 1 && depth_ - 1 < kMaxDepth && is_dict_[depth_ - 1])
                  ? absl::string_view(key_stack_[depth_ - 1]) : absl::string_view();
          handler_.onContainerOpen(push_key, to_dict, depth_, tok_start);
        } else {
          const int pop_depth = depth_;
          --depth_;
          handler_.onContainerClose(pop_depth, body_src_pos_);
          if (depth_ >= 1 && depth_ < kMaxDepth && is_dict_[depth_])
            expecting_key_[depth_] = true;
          if (depth_ >= 1 && depth_ < kMaxDepth && !is_dict_[depth_])
            ++array_index_[depth_];
        }
        break;
      }

      // JSON string segment — key or value, plain bytes only.
      // A single logical string may span multiple continued tokens if Wuffs
      // fills tok_buf_ before the closing quote; in_chain_ tracks mid-chain state.
      case WUFFS_BASE__TOKEN__VBC__STRING: {
        const absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
        if (!in_chain_) {
          // First token of a new string: decide key vs. value and pick write target.
          str_acc_.clear();
          string_is_key_ = depth_ < kMaxDepth && is_dict_[depth_] && expecting_key_[depth_];
          if (string_is_key_) {
            str_target_ = &str_acc_;
            key_tok_start_ = tok_start;
          } else {
            // key_stack_[depth_] is always current here — onKey fired before this value.
            const absl::string_view val_key =
                (depth_ < kMaxDepth && is_dict_[depth_])
                    ? absl::string_view(key_stack_[depth_]) : absl::string_view();
            str_target_ = handler_.openStringCapture(val_key, depth_, tok_start);
          }
        }
        if (str_target_ && tlen > 0) appendStringToken(*str_target_, raw, vbd);
        in_chain_ = cont;
        if (!in_chain_) {
          if (string_is_key_) {
            if (str_acc_.size() > kMaxKeyBytes) {
              return absl::InvalidArgumentError(absl::StrCat(
                  "wuffs json: key exceeds ", kMaxKeyBytes, " bytes"));
            }
            if (depth_ < kMaxDepth && !seen_keys_[depth_].insert(str_acc_).second) {
              return absl::InvalidArgumentError(
                  absl::StrCat("wuffs json: duplicate key \"", str_acc_, "\""));
            }
            if (depth_ < kMaxDepth) key_stack_[depth_] = str_acc_;
            if (auto s = handler_.onKey(str_acc_, depth_, key_tok_start_); !s.ok()) return s;
            if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
          } else {
            if (str_target_) {
              const absl::string_view val_key =
                  (depth_ < kMaxDepth && is_dict_[depth_])
                      ? absl::string_view(key_stack_[depth_]) : absl::string_view();
              handler_.closeStringCapture(str_target_, val_key, depth_, body_src_pos_);
            }
            if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          }
          str_target_ = nullptr;
        }
        break;
      }

      // Backslash escapes (\n, \t, \uXXXX, …) arrive as UNICODE_CODE_POINT tokens
      // with VBD = decoded code point.  in_chain_ is managed by surrounding STRING
      // tokens so it is not updated here.
      case WUFFS_BASE__TOKEN__VBC__UNICODE_CODE_POINT:
        if (str_target_) appendCodePoint(*str_target_, static_cast<uint32_t>(vbd));
        break;

      // NUMBER / LITERAL: dispatch to typed callbacks so the handler never
      // sees raw Wuffs token category constants.
      case WUFFS_BASE__TOKEN__VBC__NUMBER:
      case WUFFS_BASE__TOKEN__VBC__LITERAL: {
        const absl::string_view val_key =
            (depth_ < kMaxDepth && is_dict_[depth_])
                ? absl::string_view(key_stack_[depth_]) : absl::string_view();
        const absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
        if (vbc == WUFFS_BASE__TOKEN__VBC__NUMBER) {
          if (auto s = handler_.onNumber(val_key, raw, depth_, tok_start, body_src_pos_); !s.ok()) return s;
        } else if (raw == "true" || raw == "false") {
          if (auto s = handler_.onBoolean(val_key, raw[0] == 't', depth_, tok_start, body_src_pos_); !s.ok()) return s;
        } else {
          handler_.onNull(val_key, depth_, tok_start, body_src_pos_);
        }
        if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
        break;
      }

      default: break;
      }
    }

    if (status.repr == nullptr)                        { wuffs_done_ = true; break; }
    if (wuffs_base__status__is_note(&status))          { wuffs_done_ = true; break; }
    if (!wuffs_base__status__is_suspension(&status)) {
      return absl::InvalidArgumentError(
          absl::StrCat("wuffs json: ", wuffs_base__status__message(&status)));
    }
    if (status.repr == wuffs_base__suspension__short_read) break;
    tok_buf_.meta.ri = tok_buf_.meta.wi = 0; // short_write: reset ring, retry
  }
  return absl::OkStatus();
}

std::string WuffsJsonCursor::buildIndexedPath(int depth) const {
  std::string path;
  for (int d = 1; d <= depth && d < kMaxDepth; ++d) {
    if (is_dict_[d]) {
      // At the target depth, key_stack_[d] is the key currently being processed.
      // At intermediate depths, the label is the key that opened the child container
      // at d+1, stored in push_key_[d+1] at push time.
      const std::string& label = (d == depth) ? key_stack_[d] : push_key_[d + 1];
      if (!path.empty()) path += '.';
      path += label;
    } else {
      // array_index_[d] is the count of elements completed so far at this depth,
      // which equals the 0-based index of the element currently being processed.
      path += '[';
      path += std::to_string(array_index_[d]);
      path += ']';
    }
  }
  return path;
}

std::string WuffsJsonCursor::buildPatternPath(int depth) const {
  std::string path;
  for (int d = 1; d <= depth && d < kMaxDepth; ++d) {
    if (is_dict_[d]) {
      const std::string& label = (d == depth) ? key_stack_[d] : push_key_[d + 1];
      if (!path.empty()) path += '.';
      path += label;
    } else {
      path += "[]";
    }
  }
  return path;
}

} // namespace Json
} // namespace Envoy
