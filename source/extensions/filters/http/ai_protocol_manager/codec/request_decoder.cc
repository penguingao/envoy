#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

#include <cctype>
#include <cstdlib>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser  (Wuffs streaming)
//
// Replaces the old IncrementalJsonTokenizer + InferenceHandler approach with a
// Wuffs stackless coroutine.  The decoder preserves all parse state in dec_
// across feed() calls; no token_buf_ accumulation occurs.
//
// Element byte ranges for messages[]/tools[] elements are recorded during the
// streaming parse and converted to PayloadRef sub-ranges of residual_params in
// finish() — zero-copy for MmapPayloadStore, substring copy for tests.
//
// Tier 1 and Tier 2 share one feedChunk path; the only branch is captureEnabled()
// gating the byte-range recording for elements.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser {
public:
  InferenceBodyParser(const DecoderConfig& config, PayloadStore& store)
      : config_(config), store_(store) {
    dec_     = wuffs_json__decoder::alloc();
    tok_buf_ = wuffs_base__slice_token__writer(
        wuffs_base__make_slice_token(tok_data_, kTokBufLen));
  }

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
    return feedChunk(chunk, /*closed=*/false);
  }

  absl::Status finish(InferencePayload& payload, AiRequest& request) {
    auto s = feedChunk("", /*closed=*/true);
    if (!s.ok()) return s;
    if (has_error_) {
      return absl::InvalidArgumentError(
          absl::StrCat("inference body parse error: ", error_));
    }

    payload.target.name = std::move(model_);
    payload.sampling    = std::move(sampling_);
    request.streaming   = streaming_;

    if (residual_writer_) {
      payload.residual_params = residual_writer_->finalize();
    }

    // Convert recorded byte ranges to PayloadRef sub-ranges of residual_params.
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

    return absl::OkStatus();
  }

private:
  bool captureEnabled() const {
    return total_bytes_ <= config_.max_element_capture_bytes;
  }

  absl::Status feedChunk(absl::string_view chunk, bool closed) {
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
        const wuffs_base__token* tok = &tok_buf_.data.ptr[tok_buf_.meta.ri++];
        const int64_t  vbc       = wuffs_base__token__value_base_category(tok);
        const uint64_t vbd       = wuffs_base__token__value_base_detail(tok);
        const uint64_t tlen      = wuffs_base__token__length(tok);
        const bool     cont      = wuffs_base__token__continued(tok);
        const size_t   tok_start = body_src_pos_;
        body_src_pos_ += tlen;

        switch (vbc) {

        case WUFFS_BASE__TOKEN__VBC__FILLER:
          break;

        case WUFFS_BASE__TOKEN__VBC__STRUCTURE: {
          const bool is_push = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__PUSH) != 0;
          const bool to_dict = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__TO_DICT) != 0;
          if (is_push) {
            ++depth_;
            if (depth_ < kMaxDepth) {
              is_dict_[depth_]       = to_dict;
              expecting_key_[depth_] = to_dict;
            }
            if (depth_ == 2) {
              if      (current_key_ == "messages") { in_messages_ = true;  in_tools_    = false; }
              else if (current_key_ == "tools")    { in_tools_    = true;   in_messages_ = false; }
              else if (current_key_ == "stop")     { in_stop_array_ = true; }
            }
            // Tier 1: begin tracking byte range for a messages/tools element.
            if (depth_ == 3 && (in_messages_ || in_tools_) && captureEnabled()) {
              in_elem_      = true;
              elem_start_   = tok_start;
              elem_is_dict_ = to_dict;
            }
          } else {
            const int pop_depth = depth_;
            --depth_;
            // Close a messages/tools element at depth 3.
            if (pop_depth == 3 && in_elem_) {
              in_elem_ = false;
              auto kind = elem_is_dict_ ? PayloadKind::JsonObject : PayloadKind::JsonArray;
              if (in_messages_) {
                message_ranges_.push_back({elem_start_, body_src_pos_});
                message_kinds_.push_back(kind);
              } else if (in_tools_) {
                tool_ranges_.push_back({elem_start_, body_src_pos_});
                tool_kinds_.push_back(kind);
              }
            }
            if (pop_depth == 2) {
              in_messages_   = false;
              in_tools_      = false;
              in_stop_array_ = false;
            }
            if (depth_ >= 1 && depth_ < kMaxDepth && is_dict_[depth_]) {
              expecting_key_[depth_] = true;
            }
          }
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__STRING: {
          const absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
          const bool first_in_group   = !in_chain_;
          if (first_in_group) {
            str_acc_.clear();
            string_is_key_ = (depth_ < kMaxDepth && is_dict_[depth_] && expecting_key_[depth_]);
            if (string_is_key_) {
              str_target_ = &str_acc_;
            } else if (depth_ == 1) {
              if      (current_key_ == "model") { model_.clear();      str_target_ = &model_;      }
              else if (current_key_ == "stop")  { string_val_.clear(); str_target_ = &string_val_; }
              else                              { str_target_ = nullptr; }
            } else if (depth_ == 2 && in_stop_array_) {
              string_val_.clear(); str_target_ = &string_val_;
            } else {
              str_target_ = nullptr;
            }
          }
          if (str_target_ && tlen > 0) {
            appendStringToken(*str_target_, raw, vbd);
          }
          in_chain_ = cont;
          if (!cont) {
            if (string_is_key_) {
              if (depth_ == 1) {
                bool* seen = nullptr;
                if      (str_acc_ == "model")       seen = &seen_model_;
                else if (str_acc_ == "stream")      seen = &seen_stream_;
                else if (str_acc_ == "messages")    seen = &seen_messages_;
                else if (str_acc_ == "tools")       seen = &seen_tools_;
                else if (str_acc_ == "temperature") seen = &seen_temperature_;
                else if (str_acc_ == "top_p")       seen = &seen_top_p_;
                else if (str_acc_ == "max_tokens")  seen = &seen_max_tokens_;
                else if (str_acc_ == "n")           seen = &seen_n_;
                else if (str_acc_ == "seed")        seen = &seen_seed_;
                else if (str_acc_ == "stop")        seen = &seen_stop_;
                if (seen) {
                  if (*seen) {
                    has_error_ = true;
                    error_ = absl::StrCat("duplicate key \"", str_acc_,
                                          "\" in inference request body");
                    break;
                  }
                  *seen = true;
                }
                current_key_ = str_acc_;
                if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
              } else {
                if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
              }
            } else {
              if (str_target_ == &string_val_) {
                sampling_.stop.push_back(std::move(string_val_));
                string_val_.clear();
              }
              if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
            }
            str_target_ = nullptr;
          }
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__NUMBER: {
          if (depth_ == 1) {
            const absl::string_view num = chunk.substr(tok_start - chunk_base, tlen);
            int64_t i_val; double d_val;
            if      (current_key_ == "max_tokens"  && absl::SimpleAtoi(num, &i_val)) sampling_.max_tokens  = static_cast<int32_t>(i_val);
            else if (current_key_ == "n"           && absl::SimpleAtoi(num, &i_val)) sampling_.n           = static_cast<int32_t>(i_val);
            else if (current_key_ == "seed"        && absl::SimpleAtoi(num, &i_val)) sampling_.seed        = i_val;
            else if (current_key_ == "temperature" && absl::SimpleAtod(num, &d_val)) sampling_.temperature = d_val;
            else if (current_key_ == "top_p"       && absl::SimpleAtod(num, &d_val)) sampling_.top_p       = d_val;
          }
          if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__LITERAL: {
          if (depth_ == 1 && current_key_ == "stream") {
            const absl::string_view lit = chunk.substr(tok_start - chunk_base, tlen);
            if      (lit == "true")  streaming_ = true;
            else if (lit == "false") streaming_ = false;
          }
          if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          break;
        }

        default: break;
        } // switch(vbc)

        if (has_error_) break;
      } // while tokens

      if (has_error_) {
        return absl::InvalidArgumentError(
            absl::StrCat("inference body parse error: ", error_));
      }

      if (status.repr == nullptr) { wuffs_done_ = true; break; } // OK
      if (wuffs_base__status__is_note(&status)) { wuffs_done_ = true; break; }
      if (!wuffs_base__status__is_suspension(&status)) {
        return absl::InvalidArgumentError(
            absl::StrCat("wuffs json: ", wuffs_base__status__message(&status)));
      }
      if (status.repr == wuffs_base__suspension__short_read) break;
      tok_buf_.meta.ri = tok_buf_.meta.wi = 0; // short_write: reset and retry
    }

    return absl::OkStatus();
  }

  // ── Config / infra ─────────────────────────────────────────────────────────
  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  size_t                        total_bytes_{0};
  std::unique_ptr<StreamWriter> residual_writer_;

  // ── Wuffs streaming state (persists across feed() calls) ───────────────────
  wuffs_json__decoder::unique_ptr dec_;
  static constexpr size_t         kTokBufLen = 256;
  wuffs_base__token               tok_data_[kTokBufLen];
  wuffs_base__token_buffer        tok_buf_{};
  size_t                          body_src_pos_{0};
  bool                            wuffs_done_{false};

  // ── Depth / structure tracking ─────────────────────────────────────────────
  static constexpr int kMaxDepth = 8;
  int  depth_{0};
  bool is_dict_[kMaxDepth]{};
  bool expecting_key_[kMaxDepth]{};

  // ── String accumulation ────────────────────────────────────────────────────
  bool         in_chain_{false};
  bool         string_is_key_{false};
  std::string  str_acc_;
  std::string* str_target_{nullptr};
  std::string  string_val_; // scratch for stop strings

  // ── Depth-1 extracted fields ───────────────────────────────────────────────
  std::string    current_key_;
  std::string    model_;
  bool           streaming_{false};
  SamplingParams sampling_;

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

  // ── Error state ────────────────────────────────────────────────────────────
  bool        has_error_{false};
  std::string error_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser  (Wuffs streaming)
//
// Replaces the old IncrementalJsonTokenizer-based implementation.  The Wuffs
// stackless coroutine in dec_ preserves all parse state across feed() calls,
// so no token_buf_ accumulation occurs for arbitrarily large or deeply nested
// keys — each Wuffs token is at most 65535 bytes and is processed in place.
//
// Tier 1 (body ≤ max_element_capture_bytes) and Tier 2 share one code path.
// params_raw, arguments, and capabilities are recorded as byte-range sub-refs
// of residual_params — zero-copy for MmapPayloadStore, substring for tests.
// arguments/capabilities sub-ranges are only recorded when captureEnabled().
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path, const DecoderConfig& config, PayloadStore& store)
      : headers_(headers), http_method_(http_method), path_(path),
        config_(config), store_(store) {
    dec_     = wuffs_json__decoder::alloc();
    tok_buf_ = wuffs_base__slice_token__writer(
        wuffs_base__make_slice_token(tok_data_, kTokBufLen));
  }

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
    return feedChunk(chunk, /*closed=*/false);
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request) {
    auto s = feedChunk("", /*closed=*/true);
    if (!s.ok()) return s;
    if (has_error_) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent body JSON-RPC parse error: ", error_));
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

    return absl::OkStatus();
  }

private:
  bool captureEnabled() const {
    return total_bytes_ <= config_.max_element_capture_bytes;
  }

  absl::Status feedChunk(absl::string_view chunk, bool closed) {
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
        const wuffs_base__token* tok = &tok_buf_.data.ptr[tok_buf_.meta.ri++];
        const int64_t  vbc       = wuffs_base__token__value_base_category(tok);
        const uint64_t vbd       = wuffs_base__token__value_base_detail(tok);
        const uint64_t tlen      = wuffs_base__token__length(tok);
        const bool     cont      = wuffs_base__token__continued(tok);
        const size_t   tok_start = body_src_pos_;
        body_src_pos_ += tlen;

        switch (vbc) {

        case WUFFS_BASE__TOKEN__VBC__FILLER:
          break;

        case WUFFS_BASE__TOKEN__VBC__STRUCTURE: {
          const bool is_push = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__PUSH) != 0;
          const bool to_dict = (vbd & WUFFS_BASE__TOKEN__VBD__STRUCTURE__TO_DICT) != 0;
          if (is_push) {
            ++depth_;
            if (depth_ < kMaxDepth) {
              is_dict_[depth_]       = to_dict;
              expecting_key_[depth_] = to_dict;
            }
            // Entering the params container at depth 2.
            if (depth_ == 2 && current_key_ == "params") {
              in_params_         = true;
              params_byte_start_ = tok_start;
            }
            // Entering a sub-container (arguments / capabilities) at depth 3.
            if (depth_ == 3 && in_params_ && !in_sub_container_ &&
                (params_key_ == "arguments" || params_key_ == "capabilities")) {
              in_sub_container_    = true;
              sub_is_arguments_    = (params_key_ == "arguments");
              sub_container_start_ = tok_start;
              if (sub_is_arguments_) {
                arguments_kind_ = to_dict ? PayloadKind::JsonObject
                                          : PayloadKind::JsonArray;
              }
            }
          } else {
            const int pop_depth = depth_;
            --depth_;
            if (pop_depth == 3 && in_sub_container_) {
              in_sub_container_ = false;
              if (captureEnabled()) {
                if (sub_is_arguments_) {
                  arguments_byte_start_ = sub_container_start_;
                  arguments_byte_end_   = body_src_pos_;
                } else {
                  capabilities_byte_start_ = sub_container_start_;
                  capabilities_byte_end_   = body_src_pos_;
                }
              }
            }
            if (pop_depth == 2 && in_params_) {
              in_params_       = false;
              params_byte_end_ = body_src_pos_;
            }
            // After closing a container value, the parent expects the next key.
            if (depth_ >= 1 && depth_ < kMaxDepth && is_dict_[depth_]) {
              expecting_key_[depth_] = true;
            }
          }
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__STRING: {
          const absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
          const bool first_in_group = !in_chain_;
          if (first_in_group) {
            str_acc_.clear();
            string_is_key_ = (depth_ < kMaxDepth && is_dict_[depth_] &&
                               expecting_key_[depth_]);
            if (string_is_key_) {
              str_target_ = &str_acc_;
            } else if (depth_ == 1) {
              if      (current_key_ == "id")     { id_.clear();     str_target_ = &id_;     }
              else if (current_key_ == "method") { method_.clear(); str_target_ = &method_; }
              else                               { str_target_ = nullptr; }
            } else if (depth_ == 2 && in_params_) {
              if      (params_key_ == "name") { params_name_.clear(); str_target_ = &params_name_; }
              else if (params_key_ == "uri")  { params_uri_.clear();  str_target_ = &params_uri_;  }
              else if (params_key_ == "ref")  { params_ref_.clear();  str_target_ = &params_ref_;  }
              else                            { str_target_ = nullptr; }
            } else {
              str_target_ = nullptr;
            }
          }

          if (str_target_ && tlen > 0) {
            appendStringToken(*str_target_, raw, vbd);
          }
          in_chain_ = cont;

          if (!cont) {
            if (string_is_key_) {
              if (depth_ == 1) {
                bool* seen = nullptr;
                if      (str_acc_ == "jsonrpc") seen = &seen_jsonrpc_;
                else if (str_acc_ == "id")      seen = &seen_id_;
                else if (str_acc_ == "method")  seen = &seen_method_;
                else if (str_acc_ == "params")  seen = &seen_params_;
                if (seen) {
                  if (*seen) {
                    has_error_ = true;
                    error_ = absl::StrCat("duplicate key \"", str_acc_,
                                          "\" in agent request body");
                    break;
                  }
                  *seen = true;
                }
                current_key_ = str_acc_;
                if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
              } else if (depth_ == 2 && in_params_) {
                params_key_ = str_acc_;
                if (depth_ < kMaxDepth) expecting_key_[depth_] = false;
              }
            } else {
              if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
            }
            str_target_ = nullptr;
          }
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__NUMBER: {
          if (depth_ == 1 && current_key_ == "id") {
            const absl::string_view num = chunk.substr(tok_start - chunk_base, tlen);
            int64_t i_val; double d_val;
            if      (absl::SimpleAtoi(num, &i_val)) id_ = std::to_string(i_val);
            else if (absl::SimpleAtod(num, &d_val)) id_ = std::to_string(static_cast<int64_t>(d_val));
          }
          if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          break;
        }

        case WUFFS_BASE__TOKEN__VBC__LITERAL:
          if (depth_ < kMaxDepth && is_dict_[depth_]) expecting_key_[depth_] = true;
          break;

        default: break;
        } // switch(vbc)

        if (has_error_) break;
      } // while tokens

      if (has_error_) {
        return absl::InvalidArgumentError(
            absl::StrCat("agent body JSON-RPC parse error: ", error_));
      }

      if (status.repr == nullptr) { wuffs_done_ = true; break; } // OK
      // Notes (prefixed '@') signal informational events; end_of_data is one.
      if (wuffs_base__status__is_note(&status)) { wuffs_done_ = true; break; }
      if (!wuffs_base__status__is_suspension(&status)) {
        return absl::InvalidArgumentError(
            absl::StrCat("wuffs json: ", wuffs_base__status__message(&status)));
      }
      if (status.repr == wuffs_base__suspension__short_read) break; // need more input
      // short_write: token buffer full — reset and retry same source position.
      tok_buf_.meta.ri = tok_buf_.meta.wi = 0;
    }

    return absl::OkStatus();
  }

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

  // ── Config / infra ───────────────────────────────────────────────────────
  const Http::RequestHeaderMap& headers_;
  const std::string             http_method_;
  const std::string             path_;
  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  size_t                        total_bytes_{0};
  std::unique_ptr<StreamWriter> residual_writer_;

  // ── Wuffs streaming state (persists across feed() calls) ─────────────────
  wuffs_json__decoder::unique_ptr dec_;
  static constexpr size_t         kTokBufLen = 256;
  wuffs_base__token               tok_data_[kTokBufLen];
  wuffs_base__token_buffer        tok_buf_{};
  size_t                          body_src_pos_{0};
  bool                            wuffs_done_{false};

  // ── Depth / structure tracking ────────────────────────────────────────────
  static constexpr int kMaxDepth = 8;
  int  depth_{0};
  bool is_dict_[kMaxDepth]{};
  bool expecting_key_[kMaxDepth]{};

  // ── String accumulation ───────────────────────────────────────────────────
  bool         in_chain_{false};
  bool         string_is_key_{false};
  std::string  str_acc_;
  std::string* str_target_{nullptr};

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

  // ── Error state ───────────────────────────────────────────────────────────
  bool        has_error_{false};
  std::string error_;
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
