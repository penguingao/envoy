#include "source/extensions/filters/http/ai_protocol_manager/codec/wuffs_json.h"

#include <cctype>
#include <cstdlib>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"

// Wuffs declarations only (WUFFS_IMPLEMENTATION lives in wuffs_impl.c).
#include "release/c/wuffs-v0.4.c"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

// Append the decoded content of a JSON escape sequence to `out`.
// `raw` is the raw source bytes of one STRING token (which may be COPY or
// escape-encoded).  COPY bytes are appended directly; `\xNN` escapes are
// decoded to their UTF-8 representations.
void appendStringToken(std::string& out, absl::string_view raw, uint64_t vbd) {
  // DROP: source bytes produce zero output bytes (opening/closing quotes).
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) {
    return;
  }
  // Fast path: plain bytes — no backslash processing needed.
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_1_DST_1_SRC_COPY) {
    out.append(raw.data(), raw.size());
    return;
  }

  // Slow path: escape sequences in the raw source bytes.
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\' || i + 1 >= raw.size()) {
      out += raw[i];
      continue;
    }
    char esc = raw[++i];
    switch (esc) {
    case '"':  out += '"';  break;
    case '\\': out += '\\'; break;
    case '/':  out += '/';  break;
    case 'b':  out += '\b'; break;
    case 'f':  out += '\f'; break;
    case 'n':  out += '\n'; break;
    case 'r':  out += '\r'; break;
    case 't':  out += '\t'; break;
    case 'u': {
      if (i + 4 >= raw.size()) {
        break;
      }
      uint32_t cp = 0;
      for (int j = 1; j <= 4; ++j) {
        char c = raw[i + j];
        cp <<= 4;
        if (c >= '0' && c <= '9')      cp |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
      }
      i += 4;
      // Encode code point as UTF-8.
      if (cp < 0x80) {
        out += static_cast<char>(cp);
      } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      break;
    }
    default: out += esc; break;
    }
  }
}

} // namespace

// ─── WuffsJsonObjectReader::read ──────────────────────────────────────────────

absl::Status WuffsJsonObjectReader::read(absl::string_view input, Handler& handler) {
  // ── Initialize the Wuffs JSON decoder (heap-allocated via alloc()). ───────
  wuffs_json__decoder::unique_ptr dec = wuffs_json__decoder::alloc();
  if (!dec) {
    return absl::InternalError("wuffs json: alloc failed");
  }

  // ── Token buffer (stack-allocated). ───────────────────────────────────────
  static constexpr size_t kTokBufLen = 256;
  wuffs_base__token tok_data[kTokBufLen];
  wuffs_base__token_buffer tok_buf = wuffs_base__slice_token__writer(
      wuffs_base__make_slice_token(tok_data, kTokBufLen));

  // ── Source buffer pointing into the input string_view. ────────────────────
  wuffs_base__io_buffer src_buf = wuffs_base__ptr_u8__reader(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(input.data())),
      input.size(), /*closed=*/true);

  // ── Parse state. ──────────────────────────────────────────────────────────
  static constexpr int kMaxDepth = 32;
  int depth = 0;
  bool is_dict[kMaxDepth]       = {};
  bool expecting_key[kMaxDepth] = {};

  size_t src_pos = 0;       // cumulative bytes consumed from input

  std::string current_key;
  std::string str_acc;      // accumulator for string token groups
  bool in_string       = false;
  bool string_is_key   = false;
  bool in_chain        = false; // true while mid-stream on a multi-token string

  size_t container_start = 0; // source offset of current top-level container value

  // ── Main decode loop. ─────────────────────────────────────────────────────
  while (true) {
    wuffs_base__status status = wuffs_json__decoder__decode_tokens(
        dec.get(), &tok_buf, &src_buf, wuffs_base__empty_slice_u8());

    // Drain all available tokens before checking the status.
    while (tok_buf.meta.ri < tok_buf.meta.wi) {
      const wuffs_base__token* tok = &tok_buf.data.ptr[tok_buf.meta.ri++];

      const int64_t  vbc  = wuffs_base__token__value_base_category(tok);
      const uint64_t vbd  = wuffs_base__token__value_base_detail(tok);
      const uint64_t tlen = wuffs_base__token__length(tok);
      // continued() means "the next token continues this logical token".
      const bool continued = wuffs_base__token__continued(tok);

      const size_t tok_start = src_pos;
      src_pos += tlen;

      switch (vbc) {

      // ── FILLER: whitespace, colons, commas, quotes ──────────────────────
      case WUFFS_BASE__TOKEN__VBC__FILLER:
        break;

      // ── STRUCTURE: { } [ ] ──────────────────────────────────────────────
      case WUFFS_BASE__TOKEN__VBC__STRUCTURE: {
        const bool is_push = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__PUSH) != 0;
        if (is_push) {
          const bool to_dict = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__TO_DICT) != 0;
          // If this push is a top-level value (depth==1 root is open and we're
          // not expecting the next key), record the start of the raw JSON.
          if (depth == 1 && !expecting_key[1]) {
            container_start = tok_start;
          }
          ++depth;
          if (depth < kMaxDepth) {
            is_dict[depth]       = to_dict;
            expecting_key[depth] = to_dict; // objects start expecting a key
          }
        } else {
          // POP: closing } or ]
          const bool was_dict = (depth < kMaxDepth) ? is_dict[depth] : false;
          if (depth == 2 && container_start > 0) {
            // Closing a top-level container value — emit raw_json slice.
            absl::string_view raw = input.substr(container_start, src_pos - container_start);
            if (was_dict) handler.onObjectField(current_key, raw);
            else          handler.onArrayField(current_key, raw);
            container_start = 0;
            if (depth - 1 >= 0 && depth - 1 < kMaxDepth && is_dict[depth - 1]) {
              expecting_key[depth - 1] = true;
            }
          }
          --depth;
        }
        break;
      }

      // ── STRING: key strings and value strings ────────────────────────────
      case WUFFS_BASE__TOKEN__VBC__STRING: {
        if (!in_chain) {
          // First (or only) token of a new string group.
          str_acc.clear();
          in_string     = true;
          string_is_key = (depth == 1 && is_dict[1] && expecting_key[1]);
        }
        if (in_string && tlen > 0) {
          appendStringToken(str_acc, input.substr(tok_start, tlen), vbd);
        }
        if (!continued && in_string) {
          // Last (or only) token: string is complete.
          in_string = false;
          if (depth == 1) {
            if (string_is_key) {
              current_key       = std::move(str_acc);
              expecting_key[1]  = false;
            } else {
              handler.onStringField(current_key, std::move(str_acc));
              if (is_dict[1]) expecting_key[1] = true;
            }
            str_acc.clear();
          }
        }
        in_chain = continued;
        break;
      }

      // ── LITERAL: true / false / null ─────────────────────────────────────
      case WUFFS_BASE__TOKEN__VBC__LITERAL: {
        if (depth == 1 && !expecting_key[1]) {
          const absl::string_view lit = input.substr(tok_start, tlen);
          if      (lit == "null")  handler.onNullField(current_key);
          else if (lit == "true")  handler.onBoolField(current_key, true);
          else if (lit == "false") handler.onBoolField(current_key, false);
          if (is_dict[1]) expecting_key[1] = true;
        }
        break;
      }

      // ── NUMBER: integer or floating-point ─────────────────────────────────
      case WUFFS_BASE__TOKEN__VBC__NUMBER: {
        if (depth == 1 && !expecting_key[1]) {
          const absl::string_view num = input.substr(tok_start, tlen);
          int64_t i_val;
          double  d_val;
          if (absl::SimpleAtoi(num, &i_val)) {
            handler.onIntField(current_key, i_val);
          } else if (absl::SimpleAtod(num, &d_val)) {
            handler.onFloatField(current_key, d_val);
          }
          if (is_dict[1]) expecting_key[1] = true;
        }
        break;
      }

      default:
        break;
      } // switch(vbc)
    } // while tokens available

    // Decide what to do based on the decode status.
    if (status.repr == nullptr) {
      break; // done
    }
    if (!wuffs_base__status__is_suspension(&status)) {
      return absl::InvalidArgumentError(
          absl::StrCat("wuffs json: ", wuffs_base__status__message(&status)));
    }
    // Suspension: short_write means token buffer was full → flush and retry.
    tok_buf.meta.ri = tok_buf.meta.wi = 0;
  }

  return absl::OkStatus();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
