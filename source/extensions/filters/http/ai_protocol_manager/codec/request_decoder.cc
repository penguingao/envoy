#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

#include <cctype>
#include <cstdlib>
#include <tuple>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/wuffs_json.h"

// Wuffs declarations only (WUFFS_IMPLEMENTATION lives in wuffs_impl.c).
#include "release/c/wuffs-v0.4.c"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

// Decode one Wuffs STRING token's raw bytes into `out`.
// Shared by InferenceBodyParser and AgentBodyParser.
void appendStringToken(std::string& out, absl::string_view raw, uint64_t vbd) {
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) return;
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_1_DST_1_SRC_COPY) {
    out.append(raw.data(), raw.size());
    return;
  }
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\' || i + 1 >= raw.size()) { out += raw[i]; continue; }
    char esc = raw[++i];
    switch (esc) {
    case '"':  out += '"';  break;  case '\\': out += '\\'; break;
    case '/':  out += '/';  break;  case 'b':  out += '\b'; break;
    case 'f':  out += '\f'; break;  case 'n':  out += '\n'; break;
    case 'r':  out += '\r'; break;  case 't':  out += '\t'; break;
    case 'u': {
      if (i + 4 >= raw.size()) break;
      uint32_t cp = 0;
      for (int j = 1; j <= 4; ++j) {
        char c = raw[i + j]; cp <<= 4;
        if      (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
      }
      i += 4;
      // Surrogate halves (0xD800–0xDFFF) are not stitched into supplementary
      // code points. Each half is emitted as its own 3-byte CESU-8 sequence,
      // producing invalid UTF-8. This is acceptable because routing fields
      // (model, method, params.name) never contain surrogate pairs in practice.
      if      (cp < 0x80)  { out += static_cast<char>(cp); }
      else if (cp < 0x800) { out += static_cast<char>(0xC0|(cp>>6));
                             out += static_cast<char>(0x80|(cp&0x3F)); }
      else                 { out += static_cast<char>(0xE0|(cp>>12));
                             out += static_cast<char>(0x80|((cp>>6)&0x3F));
                             out += static_cast<char>(0x80|(cp&0x3F)); }
      break;
    }
    default: out += esc; break;
    }
  }
}

// Returns true for the depth-1 inference keys extracted into structured fields.
// Everything else is a passthrough field preserved for DOM-free re-encoding.
bool isExtractedInferenceKey(absl::string_view key) {
  return key == "model" || key == "stream" || key == "messages" || key == "tools" ||
         key == "temperature" || key == "top_p" || key == "max_tokens" ||
         key == "n" || key == "seed" || key == "stop";
}

// Create a sub-ref of `residual` covering [start, start+len).
// Shared by InferenceBodyParser and AgentBodyParser.
void makeSubRef(PayloadRef& ref, size_t start, size_t len,
                const PayloadRef& residual, PayloadKind kind, PayloadStore& store) {
  if (len == 0 || residual.empty()) return;
  if (residual.storage() == PayloadRef::Storage::External) {
    ref = PayloadRef::makeExternal(residual.externalOffset() + start, len);
  } else {
    std::string full = residual.toString();
    if (start < full.size()) {
      ref = store.store(full.substr(start, std::min(len, full.size() - start)), kind);
    }
  }
}

// WuffsJsonCursor wraps the Wuffs JSON tokenizer loop, depth tracking, and
// string accumulation. Callers implement Handler to receive semantic events;
// all Wuffs token mechanics live here and are invisible to the parsers.
class WuffsJsonCursor {
public:
  class Handler {
  public:
    virtual ~Handler() = default;
    // Called at the start of a non-key string chain. tok_start is the byte
    // position of the first STRING token (opening `"` is at tok_start - 1).
    // Returns the target to accumulate into, or nullptr to discard.
    virtual std::string* selectStringTarget(int depth, size_t tok_start) = 0;
    // Called when a key chain completes. key_tok_start is the position of the
    // opening `"` of the key token (includes the byte before the key content).
    // Return error to abort (e.g. duplicate key).
    virtual absl::Status onKey(absl::string_view key, int depth, size_t key_tok_start) = 0;
    // Called when a non-key string chain completes. tok_end is body_src_pos_
    // after consuming the last STRING token (position of the closing `"`).
    virtual void onStringComplete(std::string* target, int depth, size_t tok_end) = 0;
    // Called for NUMBER and LITERAL tokens. tok_start/tok_end are the byte
    // positions of the token within the accumulated body stream.
    virtual void onScalar(absl::string_view raw, int64_t vbc, int depth,
                          size_t tok_start, size_t tok_end) = 0;
    // Called after depth has been incremented for a STRUCTURE push.
    virtual void onPush(bool is_dict, int depth, size_t tok_start) = 0;
    // Called with the closing container's depth (before decrement) and the
    // body position after the closing delimiter.
    virtual void onPop(int depth, size_t tok_end) = 0;
  };

  WuffsJsonCursor(Handler& handler, bool track_paths)
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
  //                No semantic meaning for routing; silently skipped.
  //     STRUCTURE  Container open/close: { } [ ].
  //                vbd bits distinguish push vs. pop and dict vs. array:
  //                  VBD__STRUCTURE__PUSH    set on { or [
  //                  VBD__STRUCTURE__TO_DICT set on { (clear on [)
  //                depth_ is incremented before onPush, decremented after onPop,
  //                so the handler always sees the depth of the container itself.
  //     STRING     One segment of a JSON string value or key (between the quotes,
  //                with escape sequences still encoded — appendStringToken decodes
  //                them). A single logical string may span multiple tokens if
  //                Wuffs fills tok_buf_ before the closing quote; the `cont` flag
  //                (token__continued) is true on all but the last segment.
  //                vbd bits used by appendStringToken for escape decoding:
  //                  VBD__STRING__CONVERT_0_DST_1_SRC_BACKSLASH_X_*
  //                  VBD__STRING__DEFINITELY_ASCII
  //     NUMBER     A JSON number literal (integer or floating-point).
  //                Raw bytes forwarded to onScalar with vbc=NUMBER so the handler
  //                can parse with absl::SimpleAtoi / absl::SimpleAtod.
  //     LITERAL    One of: true, false, null.
  //                Raw bytes forwarded to onScalar with vbc=LITERAL so the handler
  //                can compare raw == "true" / "false" / "null".
  //
  //   vbd  (value_base_detail) — kind-specific bit flags (see vbc above).
  //
  //   tlen (token__length) — number of source bytes this token consumed.
  //        body_src_pos_ is advanced by tlen for every token regardless of vbc,
  //        giving a monotonically increasing byte counter used for byte-range
  //        recording (onPush / onPop pass tok_start / tok_end from it).
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
  absl::Status feed(absl::string_view chunk, bool closed) {
    if (!dec_) return absl::InternalError("wuffs json: alloc failed");
    if (wuffs_done_) return absl::OkStatus();

    const size_t chunk_base = body_src_pos_;
    wuffs_base__io_buffer src_buf = wuffs_base__ptr_u8__reader(
        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(chunk.data())),
        chunk.size(), closed);

    while (true) {
      wuffs_base__status status = wuffs_json__decoder__decode_tokens(
          dec_.get(), &tok_buf_, &src_buf, wuffs_base__empty_slice_u8());

      while (tok_buf_.meta.ri < tok_buf_.meta.wi) {
        const wuffs_base__token* tok      = &tok_buf_.data.ptr[tok_buf_.meta.ri++];
        const int64_t            vbc      = wuffs_base__token__value_base_category(tok);
        const uint64_t           vbd      = wuffs_base__token__value_base_detail(tok);
        const uint64_t           tlen     = wuffs_base__token__length(tok);
        const bool               cont     = wuffs_base__token__continued(tok);
        const size_t             tok_start = body_src_pos_;
        body_src_pos_ += tlen;

        switch (vbc) {
        // Whitespace, commas, colons, quote delimiters — no semantic value.
        case WUFFS_BASE__TOKEN__VBC__FILLER: break;

        // { } [ ] — container open (push) or close (pop); vbd carries push/dict bits.
        case WUFFS_BASE__TOKEN__VBC__STRUCTURE: {
          const bool is_push = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__PUSH) != 0;
          const bool to_dict = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__TO_DICT) != 0;
          if (is_push) {
            ++depth_;
            if (depth_ < kMaxDepth) {
              is_dict_[depth_]       = to_dict;
              expecting_key_[depth_] = to_dict;
              if (!to_dict) array_index_[depth_] = 0;
            }
            // push_key_[depth_] = key at parent depth that opened this container.
            // Only needed when path tracking is active (extract_fields configured).
            if (track_paths_ && depth_ <= kMaxDepth) {
              push_key_[depth_] = (depth_ > 1 && depth_ - 1 < kMaxDepth)
                                      ? key_stack_[depth_ - 1] : "";
            }
            handler_.onPush(to_dict, depth_, tok_start);
          } else {
            const int pop_depth = depth_;
            --depth_;
            handler_.onPop(pop_depth, body_src_pos_);
            if (depth_ >= 1 && depth_ < kMaxDepth && is_dict_[depth_])
              expecting_key_[depth_] = true;
            // An element just finished inside a parent array — advance the index.
            if (depth_ >= 1 && depth_ < kMaxDepth && !is_dict_[depth_])
              ++array_index_[depth_];
          }
          break;
        }

        // JSON string segment (key or value); may span multiple tokens — cont flag signals chain.
        case WUFFS_BASE__TOKEN__VBC__STRING: {
          const absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
          if (!in_chain_) {
            str_acc_.clear();
            string_is_key_ = depth_ < kMaxDepth && is_dict_[depth_] && expecting_key_[depth_];
            str_target_    = string_is_key_ ? &str_acc_ : handler_.selectStringTarget(depth_, tok_start);
            if (string_is_key_) key_tok_start_ = tok_start;
          }
          if (str_target_ && tlen > 0)
            appendStringToken(*str_target_, raw, vbd);
          in_chain_ = cont;
          if (!cont) {
            if (string_is_key_) {
              if (track_paths_ && depth_ < kMaxDepth) key_stack_[depth_] = str_acc_;
              if (auto s = handler_.onKey(str_acc_, depth_, key_tok_start_); !s.ok()) return s;
              if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
            } else {
              if (str_target_) handler_.onStringComplete(str_target_, depth_, body_src_pos_);
              if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
            }
            str_target_ = nullptr;
          }
          break;
        }

        // NUMBER: integer or float literal — handler parses raw with SimpleAtoi/SimpleAtod.
        // LITERAL: true, false, or null — handler compares raw bytes directly.
        case WUFFS_BASE__TOKEN__VBC__NUMBER:
        case WUFFS_BASE__TOKEN__VBC__LITERAL:
          handler_.onScalar(chunk.substr(tok_start - chunk_base, tlen), vbc, depth_,
                            tok_start, body_src_pos_);
          if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          break;

        default: break;
        } // switch(vbc)
      } // while tokens

      if (status.repr == nullptr)                     { wuffs_done_ = true; break; }
      if (wuffs_base__status__is_note(&status))       { wuffs_done_ = true; break; }
      if (!wuffs_base__status__is_suspension(&status)) {
        return absl::InvalidArgumentError(
            absl::StrCat("wuffs json: ", wuffs_base__status__message(&status)));
      }
      if (status.repr == wuffs_base__suspension__short_read) break;
      tok_buf_.meta.ri = tok_buf_.meta.wi = 0; // short_write: reset and retry
    }
    return absl::OkStatus();
  }

  // Builds two path representations for the string value currently being
  // selected at `depth`:
  //   indexed_path  — e.g. "messages[0].role"  (for attributes key storage)
  //   pattern_path  — e.g. "messages[].role"   (for extract_fields lookup)
  //
  // Must be called only from within a Handler callback (key_stack_ is current).
  void buildPaths(int depth, std::string& indexed_path, std::string& pattern_path) const {
    indexed_path.clear();
    pattern_path.clear();
    for (int d = 1; d <= depth && d < kMaxDepth; ++d) {
      if (is_dict_[d]) {
        // Dict level: label is the key that opened this container (push_key_[d]),
        // except at the target depth where it is the current key (key_stack_[d]).
        const std::string& label = (d == depth) ? key_stack_[d] : push_key_[d + 1];
        if (!indexed_path.empty()) indexed_path += '.';
        if (!pattern_path.empty()) pattern_path += '.';
        indexed_path += label;
        pattern_path += label;
      } else {
        // Array level: indexed uses the current element index, pattern uses [].
        indexed_path += '[';
        indexed_path += std::to_string(array_index_[d]);
        indexed_path += ']';
        pattern_path += "[]";
      }
    }
  }

private:
  Handler& handler_;
  bool     track_paths_{false};

  wuffs_json__decoder::unique_ptr dec_;
  static constexpr size_t         kTokBufLen = 256;
  wuffs_base__token               tok_data_[kTokBufLen];
  wuffs_base__token_buffer        tok_buf_{};
  size_t                          body_src_pos_{0};
  bool                            wuffs_done_{false};

  static constexpr int kMaxDepth = 8;
  int  depth_{0};
  bool is_dict_[kMaxDepth]{};
  bool expecting_key_[kMaxDepth]{};

  // Path-tracking state for buildPaths().
  // key_stack_[d]  = last key seen at depth d (set when key string chain completes).
  // push_key_[d]   = key at depth d-1 that opened the container now at depth d
  //                  (captured at push time; size kMaxDepth+1 so push_key_[kMaxDepth]
  //                  is accessible when depth_ increments to kMaxDepth).
  // array_index_[d]= count of elements already completed at array depth d
  //                  (reset on push, incremented on pop when parent is array).
  std::string key_stack_[kMaxDepth]{};
  std::string push_key_[kMaxDepth + 1]{};
  int         array_index_[kMaxDepth]{};

  bool         in_chain_{false};
  bool         string_is_key_{false};
  size_t       key_tok_start_{0};
  std::string  str_acc_;
  std::string* str_target_{nullptr};
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser  (Wuffs streaming)
//
// Thin WuffsJsonCursor::Handler subclass. The cursor owns all Wuffs mechanics;
// this class holds only domain state (routing fields, byte-range accumulators).
//
// Element byte ranges for messages[]/tools[] elements are recorded during the
// streaming parse and converted to PayloadRef sub-ranges of residual_params in
// finish() — zero-copy for MmapPayloadStore, substring copy for tests.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser : public WuffsJsonCursor::Handler {
public:
  InferenceBodyParser(const DecoderConfig& config, PayloadStore& store)
      : config_(config), store_(store),
        cursor_(*this, !config_.extract_field_pattern_set.empty()),
        min_extract_depth_(config_.min_extract_depth) {}

  absl::Status feed(absl::string_view chunk) {
    total_bytes_ += chunk.size();
    if (total_bytes_ > config_.max_body_bytes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "inference request body exceeds limit of ", config_.max_body_bytes, " bytes"));
    }
    if (!residual_writer_) {
      residual_writer_ = store_.beginStore(PayloadKind::JsonObject);
    }
    residual_writer_->append(chunk);
    return cursor_.feed(chunk, /*closed=*/false);
  }

  absl::Status finish(InferencePayload& payload, AiRequest& request) {
    auto s = cursor_.feed("", /*closed=*/true);
    if (!s.ok()) return s;

    // "model" is required by the OpenAI API. seen_model_=true with empty
    // model_ means the key was present at depth 1 but the value was not a
    // string (e.g. null or number). seen_model_=false means the key was
    // never seen at depth 1 — either absent or at an unexpected depth
    // (selectStringTarget only extracts depth-1 strings; see comment there).
    if (model_.empty()) {
      return absl::InvalidArgumentError(
          seen_model_ ? "inference request \"model\" field must be a non-empty string"
                      : "inference request body missing required field \"model\"");
    }

    payload.target.name = std::move(model_);
    payload.sampling    = std::move(sampling_);
    request.streaming   = streaming_;

    if (residual_writer_) {
      payload.residual_params = residual_writer_->finalize();
    }

    for (size_t i = 0; i < message_ranges_.size(); ++i) {
      auto [start, end] = message_ranges_[i];
      PayloadRef ref;
      makeSubRef(ref, start, end - start, payload.residual_params, message_kinds_[i], store_);
      payload.messages.push_back(std::move(ref));
    }
    for (size_t i = 0; i < tool_ranges_.size(); ++i) {
      auto [start, end] = tool_ranges_[i];
      PayloadRef ref;
      makeSubRef(ref, start, end - start, payload.residual_params, tool_kinds_[i], store_);
      payload.tools.push_back(std::move(ref));
    }
    for (auto& [k, start, end] : passthrough_ranges_) {
      PayloadRef ref;
      makeSubRef(ref, start, end - start, payload.residual_params,
                 PayloadKind::JsonObject, store_);
      payload.passthrough_fields.push_back({std::move(k), std::move(ref)});
    }
    for (auto& [k, v] : extracted_attrs_) {
      request.attributes.emplace(std::move(k), std::move(v));
    }
    return absl::OkStatus();
  }

private:
  bool captureEnabled() const { return total_bytes_ <= config_.max_element_capture_bytes; }

  // ── WuffsJsonCursor::Handler ──────────────────────────────────────────────

  // Routing scalars are extracted only at depth 1 (model, stop scalar) and
  // depth 2 inside stop[] array. This matches every known OpenAI-compatible
  // API layout (de-facto invariant, not a formal spec guarantee). A field
  // present at depth > 1 falls through to nullptr and is silently discarded;
  // finish() catches the required-field case (model) and returns an error.
  std::string* selectStringTarget(int depth, size_t tok_start) override {
    if (depth == 1) {
      if      (current_key_ == "model") { model_.clear();      return &model_; }
      else if (current_key_ == "stop")  { string_val_.clear(); return &string_val_; }
      else if (current_key_is_passthrough_) {
        passthrough_string_start_ = tok_start; // first STRING token is a DROP covering the opening "
        passthrough_string_scratch_.clear();
        return &passthrough_string_scratch_;
      }
    } else if (depth == 2 && in_stop_array_) {
      string_val_.clear();
      return &string_val_;
    }
    if (!config_.extract_field_pattern_set.empty() &&
        static_cast<size_t>(depth) >= min_extract_depth_) {
      cursor_.buildPaths(depth, path_scratch_indexed_, path_scratch_pattern_);
      if (config_.extract_field_pattern_set.contains(path_scratch_pattern_)) {
        config_field_scratch_.clear();
        std::swap(config_field_indexed_path_, path_scratch_indexed_);
        return &config_field_scratch_;
      }
    }
    return nullptr;
  }

  absl::Status onKey(absl::string_view key, int depth, size_t /*key_tok_start*/) override {
    if (depth == 1) {
      bool* seen = nullptr;
      if      (key == "model")       seen = &seen_model_;
      else if (key == "stream")      seen = &seen_stream_;
      else if (key == "messages")    seen = &seen_messages_;
      else if (key == "tools")       seen = &seen_tools_;
      else if (key == "temperature") seen = &seen_temperature_;
      else if (key == "top_p")       seen = &seen_top_p_;
      else if (key == "max_tokens")  seen = &seen_max_tokens_;
      else if (key == "n")           seen = &seen_n_;
      else if (key == "seed")        seen = &seen_seed_;
      else if (key == "stop")        seen = &seen_stop_;
      if (seen) {
        if (*seen) return absl::InvalidArgumentError(
            absl::StrCat("duplicate key \"", key, "\" in inference request body"));
        *seen = true;
      }
      current_key_              = key;
      current_key_is_passthrough_ = !isExtractedInferenceKey(key);
    }
    return absl::OkStatus();
  }

  void onStringComplete(std::string* target, int depth, size_t tok_end) override {
    if (target == &string_val_) {
      sampling_.stop.push_back(std::move(string_val_));
      string_val_.clear();
    } else if (target == &passthrough_string_scratch_ && depth == 1) {
      // tok_end is the position of the closing `"` (1 byte), so +1 for the exclusive end.
      passthrough_ranges_.push_back({current_key_, passthrough_string_start_, tok_end + 1});
    } else if (target == &config_field_scratch_) {
      extracted_attrs_.emplace_back(std::move(config_field_indexed_path_),
                                    std::move(config_field_scratch_));
    }
  }

  void onScalar(absl::string_view raw, int64_t vbc, int depth,
                size_t tok_start, size_t tok_end) override {
    if (depth == 1) {
      if (vbc == WUFFS_BASE__TOKEN__VBC__NUMBER) {
        int64_t i_val; double d_val;
        if      (current_key_ == "max_tokens"  && absl::SimpleAtoi(raw, &i_val)) sampling_.max_tokens  = static_cast<int32_t>(i_val);
        else if (current_key_ == "n"           && absl::SimpleAtoi(raw, &i_val)) sampling_.n           = static_cast<int32_t>(i_val);
        else if (current_key_ == "seed"        && absl::SimpleAtoi(raw, &i_val)) sampling_.seed        = i_val;
        else if (current_key_ == "temperature" && absl::SimpleAtod(raw, &d_val)) sampling_.temperature = d_val;
        else if (current_key_ == "top_p"       && absl::SimpleAtod(raw, &d_val)) sampling_.top_p       = d_val;
      } else if (vbc == WUFFS_BASE__TOKEN__VBC__LITERAL && current_key_ == "stream") {
        if      (raw == "true")  streaming_ = true;
        else if (raw == "false") streaming_ = false;
      }
      if (current_key_is_passthrough_) {
        passthrough_ranges_.push_back({current_key_, tok_start, tok_end});
      }
    }
    if (!config_.extract_field_pattern_set.empty() &&
        static_cast<size_t>(depth) >= min_extract_depth_) {
      cursor_.buildPaths(depth, path_scratch_indexed_, path_scratch_pattern_);
      if (config_.extract_field_pattern_set.contains(path_scratch_pattern_)) {
        extracted_attrs_.emplace_back(std::move(path_scratch_indexed_), std::string(raw));
      }
    }
  }

  void onPush(bool is_dict, int depth, size_t tok_start) override {
    if (depth == 2) {
      if      (current_key_ == "messages") { in_messages_ = true; in_tools_ = false; }
      else if (current_key_ == "tools")    { in_tools_ = true;    in_messages_ = false; }
      else if (current_key_ == "stop")     { in_stop_array_ = true; }
      else if (current_key_is_passthrough_) {
        in_passthrough_container_    = true;
        passthrough_container_start_ = tok_start;
      }
    }
    if (depth == 3 && (in_messages_ || in_tools_) && captureEnabled()) {
      in_elem_      = true;
      elem_start_   = tok_start;
      elem_is_dict_ = is_dict;
    }
  }

  void onPop(int depth, size_t tok_end) override {
    if (depth == 3 && in_elem_) {
      in_elem_ = false;
      const auto kind = elem_is_dict_ ? PayloadKind::JsonObject : PayloadKind::JsonArray;
      if (in_messages_) {
        message_ranges_.push_back({elem_start_, tok_end});
        message_kinds_.push_back(kind);
      } else if (in_tools_) {
        tool_ranges_.push_back({elem_start_, tok_end});
        tool_kinds_.push_back(kind);
      }
    }
    if (depth == 2) {
      if (in_passthrough_container_) {
        passthrough_ranges_.push_back({current_key_, passthrough_container_start_, tok_end});
      }
      in_messages_              = false;
      in_tools_                 = false;
      in_stop_array_            = false;
      in_passthrough_container_ = false;
    }
  }

  // ── Config / infra ─────────────────────────────────────────────────────────
  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  WuffsJsonCursor               cursor_;
  size_t                        total_bytes_{0};
  std::unique_ptr<StreamWriter> residual_writer_;

  // ── Config-driven field extraction ─────────────────────────────────────────
  // min_extract_depth_: skip buildPaths entirely for shallow fields.
  // path_scratch_*: reused across calls — avoids per-call heap alloc once warmed.
  // extracted_attrs_: vector-of-pairs so both key and value are movable at finish().
  size_t                                        min_extract_depth_;
  std::string                                   path_scratch_indexed_;
  std::string                                   path_scratch_pattern_;
  std::string                                   config_field_scratch_;
  std::string                                   config_field_indexed_path_;
  std::vector<std::pair<std::string, std::string>> extracted_attrs_;

  // ── Extracted fields ───────────────────────────────────────────────────────
  std::string    current_key_;
  std::string    model_;
  bool           streaming_{false};
  SamplingParams sampling_;
  std::string    string_val_; // scratch for stop strings

  // ── Container / element tracking ──────────────────────────────────────────
  bool   in_messages_{false};
  bool   in_tools_{false};
  bool   in_stop_array_{false};
  bool   in_elem_{false};
  bool   elem_is_dict_{false};
  size_t elem_start_{0};

  std::vector<std::pair<size_t, size_t>> message_ranges_;
  std::vector<PayloadKind>               message_kinds_;
  std::vector<std::pair<size_t, size_t>> tool_ranges_;
  std::vector<PayloadKind>               tool_kinds_;

  // ── Passthrough field tracking ─────────────────────────────────────────────
  bool        current_key_is_passthrough_{false};
  std::string passthrough_string_scratch_; // non-null sentinel for onStringComplete
  size_t      passthrough_string_start_{0}; // byte position of opening `"` in residual
  bool        in_passthrough_container_{false};
  size_t      passthrough_container_start_{0};
  // All passthrough values (strings, scalars, containers): (key, start, end) in residual.
  std::vector<std::tuple<std::string, size_t, size_t>> passthrough_ranges_;

  // ── Duplicate-key tracking ─────────────────────────────────────────────────
  bool seen_model_{false};
  bool seen_stream_{false};
  bool seen_messages_{false};
  bool seen_tools_{false};
  bool seen_temperature_{false};
  bool seen_top_p_{false};
  bool seen_max_tokens_{false};
  bool seen_n_{false};
  bool seen_seed_{false};
  bool seen_stop_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser  (Wuffs streaming)
//
// Thin WuffsJsonCursor::Handler subclass. The cursor owns all Wuffs mechanics;
// this class holds only domain state (method, id, params byte-ranges).
//
// params_raw, arguments, and capabilities are recorded as byte-range sub-refs
// of residual_params — zero-copy for MmapPayloadStore, substring for tests.
// arguments/capabilities sub-ranges are only recorded when captureEnabled().
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser : public WuffsJsonCursor::Handler {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path, const DecoderConfig& config, PayloadStore& store)
      : headers_(headers), http_method_(http_method), path_(path),
        config_(config), store_(store),
        cursor_(*this, !config_.extract_field_pattern_set.empty()),
        min_extract_depth_(config_.min_extract_depth) {}

  absl::Status feed(absl::string_view chunk) {
    total_bytes_ += chunk.size();
    if (total_bytes_ > config_.max_body_bytes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "agent request body exceeds limit of ", config_.max_body_bytes, " bytes"));
    }
    if (!residual_writer_) {
      residual_writer_ = store_.beginStore(PayloadKind::JsonObject);
    }
    residual_writer_->append(chunk);
    return cursor_.feed(chunk, /*closed=*/false);
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request) {
    auto s = cursor_.feed("", /*closed=*/true);
    if (!s.ok()) return s;

    // seen_method_=true: "method" key was present at depth 1, but the value
    // was not a string (e.g. null or number) — explicit JSON-RPC violation.
    // seen_method_=false with empty method_: key absent or at unexpected depth
    // (selectStringTarget only extracts depth-1 strings; see comment there) —
    // indistinguishable from webhook false-positive; falls through below.
    if (seen_method_ && method_.empty()) {
      return absl::InvalidArgumentError(
          "agent request \"method\" field must be a non-empty string");
    }

    request.jsonrpc_id = std::move(id_);
    request.rpc_method = std::move(method_);

    if (!request.rpc_method.empty()) {
      ClassifyResult r2 = classify({http_method_, path_, headers_, request.rpc_method});
      request.protocol  = r2.protocol;
      if (auto* inv = std::get_if<AgentInvocation>(&r2.invocation)) {
        payload.invocation = *inv;
      }
      payload.dialect = (r2.protocol == ProtocolKind::AgenticMcp) ? AgentDialect::Mcp
                                                                   : AgentDialect::A2a;
    }

    populatePayload(payload);

    if (residual_writer_) {
      payload.residual_params = residual_writer_->finalize();
    }

    if (params_byte_end_ > params_byte_start_) {
      makeSubRef(payload.params_raw, params_byte_start_,
                 params_byte_end_ - params_byte_start_,
                 payload.residual_params, PayloadKind::JsonObject, store_);
    }
    if (arguments_byte_end_ > arguments_byte_start_) {
      makeSubRef(payload.arguments, arguments_byte_start_,
                 arguments_byte_end_ - arguments_byte_start_,
                 payload.residual_params, arguments_kind_, store_);
    }
    if (capabilities_byte_end_ > capabilities_byte_start_) {
      makeSubRef(payload.capabilities, capabilities_byte_start_,
                 capabilities_byte_end_ - capabilities_byte_start_,
                 payload.residual_params, PayloadKind::JsonObject, store_);
    }
    for (auto& [k, v] : extracted_attrs_) {
      request.attributes.emplace(std::move(k), std::move(v));
    }

    return absl::OkStatus();
  }

private:
  bool captureEnabled() const { return total_bytes_ <= config_.max_element_capture_bytes; }

  void populatePayload(AgentPayload& payload) const {
    switch (payload.invocation) {
    case AgentInvocation::ToolsCall:
      payload.tool_name = params_name_;
      break;
    case AgentInvocation::ResourcesRead:
    case AgentInvocation::ResourcesSubscribe:
    case AgentInvocation::ResourcesUnsubscribe:
      payload.resource_uri = params_uri_;
      break;
    case AgentInvocation::PromptsGet:
      payload.prompt_name = params_name_;
      break;
    case AgentInvocation::CompletionComplete:
      payload.completion_ref = params_ref_;
      break;
    default:
      break;
    }
  }

  // ── WuffsJsonCursor::Handler ──────────────────────────────────────────────

  // Routing scalars are extracted at depth 1 (id, method) and depth 2 within
  // params (name, uri, ref), matching JSON-RPC 2.0 and current MCP/A2A specs.
  // Fields at other depths fall through to nullptr and are silently discarded.
  // Empty method_ is propagated to finish() and treated as non-AI traffic by
  // the caller — this covers both the webhook false-positive case and the
  // (silent) depth-mismatch case where "method" appears outside depth 1.
  std::string* selectStringTarget(int depth, size_t /*tok_start*/) override {
    if (depth == 1) {
      if      (current_key_ == "id")     { id_.clear();     return &id_; }
      else if (current_key_ == "method") { method_.clear(); return &method_; }
    } else if (depth == 2 && in_params_) {
      if      (params_key_ == "name") { params_name_.clear(); return &params_name_; }
      else if (params_key_ == "uri")  { params_uri_.clear();  return &params_uri_; }
      else if (params_key_ == "ref")  { params_ref_.clear();  return &params_ref_; }
    }
    if (!config_.extract_field_pattern_set.empty() &&
        static_cast<size_t>(depth) >= min_extract_depth_) {
      cursor_.buildPaths(depth, path_scratch_indexed_, path_scratch_pattern_);
      if (config_.extract_field_pattern_set.contains(path_scratch_pattern_)) {
        config_field_scratch_.clear();
        std::swap(config_field_indexed_path_, path_scratch_indexed_);
        return &config_field_scratch_;
      }
    }
    return nullptr;
  }

  absl::Status onKey(absl::string_view key, int depth, size_t /*key_tok_start*/) override {
    if (depth == 1) {
      bool* seen = nullptr;
      if      (key == "jsonrpc") seen = &seen_jsonrpc_;
      else if (key == "id")      seen = &seen_id_;
      else if (key == "method")  seen = &seen_method_;
      else if (key == "params")  seen = &seen_params_;
      if (seen) {
        if (*seen) return absl::InvalidArgumentError(
            absl::StrCat("duplicate key \"", key, "\" in agent request body"));
        *seen = true;
      }
      current_key_ = key;
    } else if (depth == 2 && in_params_) {
      params_key_ = key;
    }
    return absl::OkStatus();
  }

  void onStringComplete(std::string* target, int /*depth*/, size_t /*tok_end*/) override {
    if (target == &config_field_scratch_) {
      extracted_attrs_.emplace_back(std::move(config_field_indexed_path_),
                                    std::move(config_field_scratch_));
    }
  }

  void onScalar(absl::string_view raw, int64_t vbc, int depth,
                size_t /*tok_start*/, size_t /*tok_end*/) override {
    if (depth == 1 && current_key_ == "id" && vbc == WUFFS_BASE__TOKEN__VBC__NUMBER) {
      int64_t i_val; double d_val;
      if      (absl::SimpleAtoi(raw, &i_val)) id_ = std::to_string(i_val);
      else if (absl::SimpleAtod(raw, &d_val)) id_ = std::to_string(static_cast<int64_t>(d_val));
    }
    if (!config_.extract_field_pattern_set.empty() &&
        static_cast<size_t>(depth) >= min_extract_depth_) {
      cursor_.buildPaths(depth, path_scratch_indexed_, path_scratch_pattern_);
      if (config_.extract_field_pattern_set.contains(path_scratch_pattern_)) {
        extracted_attrs_.emplace_back(std::move(path_scratch_indexed_), std::string(raw));
      }
    }
  }

  void onPush(bool is_dict, int depth, size_t tok_start) override {
    if (depth == 2 && current_key_ == "params") {
      in_params_         = true;
      params_byte_start_ = tok_start;
    }
    if (depth == 3 && in_params_ && !in_sub_container_ &&
        (params_key_ == "arguments" || params_key_ == "capabilities")) {
      in_sub_container_    = true;
      sub_is_arguments_    = (params_key_ == "arguments");
      sub_container_start_ = tok_start;
      if (sub_is_arguments_) {
        arguments_kind_ = is_dict ? PayloadKind::JsonObject : PayloadKind::JsonArray;
      }
    }
  }

  void onPop(int depth, size_t tok_end) override {
    if (depth == 3 && in_sub_container_) {
      in_sub_container_ = false;
      if (captureEnabled()) {
        if (sub_is_arguments_) {
          arguments_byte_start_ = sub_container_start_;
          arguments_byte_end_   = tok_end;
        } else {
          capabilities_byte_start_ = sub_container_start_;
          capabilities_byte_end_   = tok_end;
        }
      }
    }
    if (depth == 2 && in_params_) {
      in_params_       = false;
      params_byte_end_ = tok_end;
    }
  }

  // ── Config / infra ───────────────────────────────────────────────────────
  const Http::RequestHeaderMap& headers_;
  const std::string             http_method_;
  const std::string             path_;
  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  WuffsJsonCursor               cursor_;
  size_t                        total_bytes_{0};
  std::unique_ptr<StreamWriter> residual_writer_;

  // ── Config-driven field extraction ─────────────────────────────────────────
  size_t                                           min_extract_depth_;
  std::string                                      path_scratch_indexed_;
  std::string                                      path_scratch_pattern_;
  std::string                                      config_field_scratch_;
  std::string                                      config_field_indexed_path_;
  std::vector<std::pair<std::string, std::string>> extracted_attrs_;

  // ── Top-level extracted fields ────────────────────────────────────────────
  std::string current_key_;
  std::string id_;
  std::string method_;

  // ── params byte-range tracking ────────────────────────────────────────────
  bool   in_params_{false};
  size_t params_byte_start_{0};
  size_t params_byte_end_{0};

  // ── params sub-field extraction ───────────────────────────────────────────
  std::string params_key_;
  std::string params_name_;
  std::string params_uri_;
  std::string params_ref_;

  // ── sub-container (arguments / capabilities) byte-range tracking ──────────
  bool        in_sub_container_{false};
  bool        sub_is_arguments_{false};
  size_t      sub_container_start_{0};
  size_t      arguments_byte_start_{0};
  size_t      arguments_byte_end_{0};
  size_t      capabilities_byte_start_{0};
  size_t      capabilities_byte_end_{0};
  PayloadKind arguments_kind_{PayloadKind::JsonObject};

  // ── Duplicate-key tracking ────────────────────────────────────────────────
  bool seen_jsonrpc_{false};
  bool seen_id_{false};
  bool seen_method_{false};
  bool seen_params_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// RequestDecoder
// ─────────────────────────────────────────────────────────────────────────────

RequestDecoder::~RequestDecoder() = default;

RequestDecoder::RequestDecoder(const DecoderConfig& config, PayloadStore& store)
    : config_(config), payload_store_(store) {}

void RequestDecoder::populateQueryParams(absl::string_view path) {
  auto q = path.find('?');
  if (q == absl::string_view::npos) {
    return;
  }
  absl::string_view query = path.substr(q + 1);
  for (absl::string_view pair : absl::StrSplit(query, '&')) {
    auto eq = pair.find('=');
    if (eq == absl::string_view::npos) {
      request_.query_params[std::string(pair)] = "";
    } else {
      request_.query_params[std::string(pair.substr(0, eq))] =
          std::string(pair.substr(eq + 1));
    }
  }
}

absl::Status RequestDecoder::populateBodilessPayload(const ClassifyResult& result) {
  switch (result.protocol) {
  case ProtocolKind::Inference: {
    InferencePayload payload;
    if (auto* inv = std::get_if<InferenceInvocation>(&result.invocation)) {
      payload.invocation = *inv;
    }
    if (auto it = result.path_params.find("id"); it != result.path_params.end()) {
      payload.resource_id = it->second;
    }
    request_.payload = std::move(payload);
    break;
  }
  case ProtocolKind::AgenticMcp: {
    AgentPayload payload;
    payload.dialect = AgentDialect::Mcp;
    if (auto* inv = std::get_if<AgentInvocation>(&result.invocation)) {
      payload.invocation = *inv;
    }
    request_.payload = std::move(payload);
    break;
  }
  case ProtocolKind::AgenticA2a: {
    AgentPayload payload;
    payload.dialect = AgentDialect::A2a;
    if (auto* inv = std::get_if<AgentInvocation>(&result.invocation)) {
      payload.invocation = *inv;
    }
    request_.payload = std::move(payload);
    break;
  }
  default:
    break;
  }
  return absl::OkStatus();
}

absl::Status RequestDecoder::onHeaders(const Http::RequestHeaderMap& headers) {
  ASSERT(state_ == DecodeState::AwaitingHeaders);

  request_.http_method = std::string(headers.getMethodValue());
  request_.path        = std::string(headers.getPathValue());
  request_.headers     = const_cast<Http::RequestHeaderMap*>(&headers);

  populateQueryParams(request_.path);

  ClassifyResult result = classify({
      request_.http_method,
      request_.path,
      headers,
      "",
  });
  request_.protocol = result.protocol;
  for (auto& [k, v] : result.path_params) {
    request_.path_params[k] = std::move(v);
  }

  const absl::string_view cl  = headers.getContentLengthValue();
  const absl::string_view te  = headers.getTransferEncodingValue();
  const bool body_hinted_by_headers =
      (!cl.empty() && cl != "0") || !te.empty();
  const bool body_bearing_verb =
      request_.http_method == "POST" || request_.http_method == "PUT" ||
      request_.http_method == "PATCH";
  const bool expects_body = body_bearing_verb || body_hinted_by_headers;

  if (!expects_body) {
    auto status = populateBodilessPayload(result);
    if (!status.ok()) {
      state_ = DecodeState::Error;
      return status;
    }
    state_ = DecodeState::BodilessComplete;
    return absl::OkStatus();
  }

  if (result.protocol == ProtocolKind::Inference) {
    InferencePayload payload;
    if (auto* inv = std::get_if<InferenceInvocation>(&result.invocation)) {
      payload.invocation = *inv;
    }
    auto ph = headers.get(Http::LowerCaseString("x-ai-provider"));
    if (!ph.empty()) {
      payload.target.provider_hint = std::string(ph[0]->value().getStringView());
    }
    request_.payload  = std::move(payload);
    inference_parser_ = std::make_unique<InferenceBodyParser>(config_, payload_store_);
    state_            = DecodeState::ParsingInferenceBody;
  } else {
    AgentPayload payload;
    if (result.protocol == ProtocolKind::AgenticA2a) {
      payload.dialect = AgentDialect::A2a;
    } else if (result.protocol == ProtocolKind::AgenticMcp) {
      payload.dialect = AgentDialect::Mcp;
    }
    request_.payload = std::move(payload);
    agent_parser_    = std::make_unique<AgentBodyParser>(headers, request_.http_method,
                                                         request_.path, config_,
                                                         payload_store_);
    state_           = DecodeState::ParsingAgentBody;
  }

  return absl::OkStatus();
}

absl::Status RequestDecoder::onData(absl::string_view chunk) {
  switch (state_) {
  case DecodeState::ParsingInferenceBody:
    ASSERT(inference_parser_ != nullptr);
    if (auto s = inference_parser_->feed(chunk); !s.ok()) {
      state_ = DecodeState::Error;
      return s;
    }
    return absl::OkStatus();

  case DecodeState::ParsingAgentBody:
    ASSERT(agent_parser_ != nullptr);
    if (auto s = agent_parser_->feed(chunk); !s.ok()) {
      state_ = DecodeState::Error;
      return s;
    }
    return absl::OkStatus();

  default:
    return absl::OkStatus();
  }
}

absl::Status RequestDecoder::onTrailers(const Http::RequestTrailerMap& /*trailers*/) {
  return onEndStream();
}

absl::Status RequestDecoder::onEndStream() {
  switch (state_) {
  case DecodeState::BodilessComplete:
    state_ = DecodeState::BodyComplete;
    break;

  case DecodeState::ParsingInferenceBody: {
    ASSERT(inference_parser_ != nullptr);
    auto* payload = request_.as_inference();
    ASSERT(payload != nullptr);
    auto status = inference_parser_->finish(*payload, request_);
    if (!status.ok()) {
      state_ = DecodeState::Error;
      return status;
    }
    state_ = DecodeState::BodyComplete;
    break;
  }

  case DecodeState::ParsingAgentBody: {
    ASSERT(agent_parser_ != nullptr);
    auto* payload = request_.as_agent();
    ASSERT(payload != nullptr);
    auto status = agent_parser_->finish(*payload, request_);
    if (!status.ok()) {
      state_ = DecodeState::Error;
      return status;
    }
    state_ = DecodeState::BodyComplete;
    break;
  }

  default:
    return absl::InternalError("RequestDecoder::onEndStream called in unexpected state");
  }

  request_.payload_store = &payload_store_;
  return absl::OkStatus();
}

bool RequestDecoder::needsBody() const {
  return state_ == DecodeState::ParsingInferenceBody ||
         state_ == DecodeState::ParsingAgentBody;
}

absl::StatusOr<AiRequest> RequestDecoder::take() {
  if (state_ != DecodeState::BodyComplete) {
    return absl::FailedPreconditionError(
        "RequestDecoder::take() called before successful onEndStream()");
  }
  AiRequest result = std::move(request_);
  request_ = AiRequest{};
  state_   = DecodeState::AwaitingHeaders;
  inference_parser_.reset();
  agent_parser_.reset();
  return result;
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
