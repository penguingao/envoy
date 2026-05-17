#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

#include <cctype>
#include <cstdlib>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

using nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// IncrementalJsonTokenizer
//
// Incremental-input SAX-output JSON tokenizer. feed() can be called repeatedly
// with arbitrary byte chunks; all parse state lives in explicit data members
// (not the call stack) so parsing can suspend and resume at any chunk boundary.
//
// Two modes:
//
//   Semantic mode  — fires Handler callbacks as tokens complete.  The handler
//   may call startCapture(writer) from within onStartObject() or onStartArray()
//   to switch the current container into capture mode.
//
//   Capture mode   — all raw bytes are forwarded verbatim to the StreamWriter
//   until the captured container's matching close delimiter is seen.
//   onEndObject() or onEndArray() fires once for that close; no semantic events
//   fire for nested content inside the captured container.
// ─────────────────────────────────────────────────────────────────────────────
class IncrementalJsonTokenizer {
public:
  struct Handler {
    virtual ~Handler() = default;
    // depth_ is already incremented inside the handler before these return.
    virtual bool onKey(absl::string_view key)  = 0;
    virtual bool onString(absl::string_view v) = 0;
    virtual bool onInt(int64_t v)              = 0;
    virtual bool onFloat(double v)             = 0;
    virtual bool onBool(bool v)                = 0;
    virtual bool onNull()                      = 0;
    virtual bool onStartObject()               = 0;
    virtual bool onEndObject()                 = 0;
    virtual bool onStartArray()                = 0;
    virtual bool onEndArray()                  = 0;
  };

  explicit IncrementalJsonTokenizer(Handler& h) : handler_(h) {}

  // Switch the current container into capture mode.  Must be called from
  // within an onStartObject() or onStartArray() callback.  The opening
  // delimiter is written to writer immediately; all subsequent bytes are
  // forwarded until the container closes, at which point onEndObject() or
  // onEndArray() fires and the tokenizer returns to semantic mode.
  void startCapture(StreamWriter& writer) {
    ASSERT(pending_capture_);
    pending_capture_   = false;
    capture_writer_    = &writer;
    cap_depth_counter_ = 1;
    cap_in_string_     = false;
    cap_in_escape_     = false;
    writer.append({&capture_open_char_, 1});
    state_ = ParseState::InCapture;
  }

  absl::Status feed(absl::string_view chunk);
  absl::Status finish(); // error if parse is incomplete at end of input

  int depth() const { return depth_; }

private:
  enum class ParseState {
    ExpectValue,        // start of document / after '[' / after ':'
    ExpectKeyOrClose,   // after '{' or ',' inside an object
    ExpectColon,        // after a key string
    ExpectCommaOrClose, // after any value
    InKey,              // inside a key string
    InKeyEscape,        // after '\' inside a key string
    InKeyUnicode,       // consuming 4 hex digits of \u in a key
    InStringValue,      // inside a value string
    InStringEscape,     // after '\' inside a value string
    InStringUnicode,    // consuming 4 hex digits of \u in a value
    InNumber,           // inside a number token
    InKeyword,          // inside true / false / null
    InCapture,          // forwarding raw bytes to capture_writer_
    Done,
  };

  absl::Status processByte(unsigned char c, bool& reprocess);
  absl::Status fireNumber();
  absl::Status fireKeyword();

  Handler&   handler_;
  ParseState state_{ParseState::ExpectValue};

  static constexpr int kMaxDepth = 64;
  int  depth_{0};
  bool is_object_[kMaxDepth]{}; // true = object at that depth, false = array

  std::string token_buf_;
  int         unicode_count_{0};

  bool  pending_capture_{false};
  char  capture_open_char_{0};
  StreamWriter* capture_writer_{nullptr};
  int   cap_depth_counter_{0};
  bool  cap_in_string_{false};
  bool  cap_in_escape_{false};
};

// ─── feed / finish ───────────────────────────────────────────────────────────

absl::Status IncrementalJsonTokenizer::feed(absl::string_view chunk) {
  for (size_t i = 0; i < chunk.size(); ) {
    bool reprocess = false;
    auto s = processByte(static_cast<unsigned char>(chunk[i]), reprocess);
    if (!s.ok()) return s;
    if (!reprocess) ++i;
  }
  return absl::OkStatus();
}

absl::Status IncrementalJsonTokenizer::finish() {
  // Numbers have no terminator; flush whatever is accumulated.
  if (state_ == ParseState::InNumber) {
    auto s = fireNumber();
    if (!s.ok()) return s;
    state_ = ParseState::Done;
  }
  if (state_ == ParseState::Done || state_ == ParseState::ExpectValue) {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("incomplete JSON at end of input (state=",
                   static_cast<int>(state_), ")"));
}

// ─── scalar helpers ──────────────────────────────────────────────────────────

absl::Status IncrementalJsonTokenizer::fireNumber() {
  const std::string& s = token_buf_;
  bool is_float = s.find('.') != std::string::npos ||
                  s.find('e') != std::string::npos ||
                  s.find('E') != std::string::npos;
  if (!is_float) {
    char* end;
    long long iv = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() + s.size()) {
      token_buf_.clear();
      if (!handler_.onInt(static_cast<int64_t>(iv)))
        return absl::InvalidArgumentError("handler rejected integer value");
      return absl::OkStatus();
    }
  }
  char* end;
  double fv = std::strtod(s.c_str(), &end);
  if (end == s.c_str() + s.size()) {
    token_buf_.clear();
    if (!handler_.onFloat(fv))
      return absl::InvalidArgumentError("handler rejected float value");
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(absl::StrCat("invalid number token: ", s));
}

absl::Status IncrementalJsonTokenizer::fireKeyword() {
  bool ok;
  if      (token_buf_ == "true")  { ok = handler_.onBool(true);  }
  else if (token_buf_ == "false") { ok = handler_.onBool(false); }
  else if (token_buf_ == "null")  { ok = handler_.onNull();      }
  else {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid JSON keyword: ", token_buf_));
  }
  token_buf_.clear();
  if (!ok) return absl::InvalidArgumentError("handler rejected keyword");
  return absl::OkStatus();
}

// ─── main state machine ──────────────────────────────────────────────────────

absl::Status IncrementalJsonTokenizer::processByte(unsigned char c, bool& reprocess) {
  reprocess = false;

  switch (state_) {

  // ── Expect start of a JSON value ──────────────────────────────────────────
  case ParseState::ExpectValue:
    if (std::isspace(c)) break;
    if (c == '{') {
      if (depth_ >= kMaxDepth)
        return absl::ResourceExhaustedError("JSON nesting exceeds limit");
      ++depth_;
      is_object_[depth_] = true;
      capture_open_char_ = static_cast<char>(c);
      pending_capture_   = true;
      if (!handler_.onStartObject())
        return absl::InvalidArgumentError("handler error in onStartObject");
      if (state_ != ParseState::InCapture) {
        pending_capture_ = false;
        state_ = ParseState::ExpectKeyOrClose;
      }
    } else if (c == '[') {
      if (depth_ >= kMaxDepth)
        return absl::ResourceExhaustedError("JSON nesting exceeds limit");
      ++depth_;
      is_object_[depth_] = false;
      capture_open_char_ = static_cast<char>(c);
      pending_capture_   = true;
      if (!handler_.onStartArray())
        return absl::InvalidArgumentError("handler error in onStartArray");
      if (state_ != ParseState::InCapture) {
        pending_capture_ = false;
        state_ = ParseState::ExpectValue;
      }
    } else if (c == ']') {
      // Empty array: ']' immediately after '['.
      --depth_;
      if (!handler_.onEndArray())
        return absl::InvalidArgumentError("handler error in onEndArray");
      state_ = (depth_ == 0) ? ParseState::Done : ParseState::ExpectCommaOrClose;
    } else if (c == '"') {
      token_buf_.clear();
      state_ = ParseState::InStringValue;
    } else if (c == '-' || std::isdigit(c)) {
      token_buf_.clear();
      token_buf_.push_back(static_cast<char>(c));
      state_ = ParseState::InNumber;
    } else if (c == 't' || c == 'f' || c == 'n') {
      token_buf_.clear();
      token_buf_.push_back(static_cast<char>(c));
      state_ = ParseState::InKeyword;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("unexpected byte 0x",
                       absl::Hex(c), " in value position"));
    }
    break;

  // ── Inside an object: expect a key string or closing '}' ─────────────────
  case ParseState::ExpectKeyOrClose:
    if (std::isspace(c)) break;
    if (c == '}') {
      --depth_;
      if (!handler_.onEndObject())
        return absl::InvalidArgumentError("handler error in onEndObject");
      state_ = (depth_ == 0) ? ParseState::Done : ParseState::ExpectCommaOrClose;
    } else if (c == '"') {
      token_buf_.clear();
      state_ = ParseState::InKey;
    } else {
      return absl::InvalidArgumentError("expected key string or '}'");
    }
    break;

  // ── Expect ':' after a key ────────────────────────────────────────────────
  case ParseState::ExpectColon:
    if (std::isspace(c)) break;
    if (c == ':') {
      state_ = ParseState::ExpectValue;
    } else {
      return absl::InvalidArgumentError("expected ':' after key");
    }
    break;

  // ── Expect ',' or closing delimiter after a value ─────────────────────────
  case ParseState::ExpectCommaOrClose:
    if (std::isspace(c)) break;
    if (c == ',') {
      state_ = (depth_ > 0 && is_object_[depth_])
                   ? ParseState::ExpectKeyOrClose
                   : ParseState::ExpectValue;
    } else if (c == '}') {
      --depth_;
      if (!handler_.onEndObject())
        return absl::InvalidArgumentError("handler error in onEndObject");
      state_ = (depth_ == 0) ? ParseState::Done : ParseState::ExpectCommaOrClose;
    } else if (c == ']') {
      --depth_;
      if (!handler_.onEndArray())
        return absl::InvalidArgumentError("handler error in onEndArray");
      state_ = (depth_ == 0) ? ParseState::Done : ParseState::ExpectCommaOrClose;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("expected ',', '}', or ']' but got 0x", absl::Hex(c)));
    }
    break;

  // ── Key string ────────────────────────────────────────────────────────────
  case ParseState::InKey:
    if (c == '"') {
      if (!handler_.onKey(token_buf_))
        return absl::InvalidArgumentError("handler error in onKey");
      token_buf_.clear();
      state_ = ParseState::ExpectColon;
    } else if (c == '\\') {
      state_ = ParseState::InKeyEscape;
    } else if (c >= 0x20) {
      token_buf_.push_back(static_cast<char>(c));
    } else {
      return absl::InvalidArgumentError("control character inside key string");
    }
    break;

  case ParseState::InKeyEscape:
    switch (c) {
    case '"':  token_buf_.push_back('"');  break;
    case '\\': token_buf_.push_back('\\'); break;
    case '/':  token_buf_.push_back('/');  break;
    case 'b':  token_buf_.push_back('\b'); break;
    case 'f':  token_buf_.push_back('\f'); break;
    case 'n':  token_buf_.push_back('\n'); break;
    case 'r':  token_buf_.push_back('\r'); break;
    case 't':  token_buf_.push_back('\t'); break;
    case 'u':
      unicode_count_ = 0;
      state_ = ParseState::InKeyUnicode;
      return absl::OkStatus();
    default:
      return absl::InvalidArgumentError("invalid escape in key string");
    }
    state_ = ParseState::InKey;
    break;

  case ParseState::InKeyUnicode:
    // Field names we match are plain ASCII; just consume the 4 hex digits
    // and append a placeholder so the key won't match any known field name.
    if (++unicode_count_ == 4) {
      token_buf_.push_back('?');
      state_ = ParseState::InKey;
    }
    break;

  // ── Value string ─────────────────────────────────────────────────────────
  case ParseState::InStringValue:
    if (c == '"') {
      if (!handler_.onString(token_buf_))
        return absl::InvalidArgumentError("handler error in onString");
      token_buf_.clear();
      state_ = ParseState::ExpectCommaOrClose;
    } else if (c == '\\') {
      state_ = ParseState::InStringEscape;
    } else if (c >= 0x20) {
      token_buf_.push_back(static_cast<char>(c));
    } else {
      return absl::InvalidArgumentError("control character inside string value");
    }
    break;

  case ParseState::InStringEscape:
    switch (c) {
    case '"':  token_buf_.push_back('"');  break;
    case '\\': token_buf_.push_back('\\'); break;
    case '/':  token_buf_.push_back('/');  break;
    case 'b':  token_buf_.push_back('\b'); break;
    case 'f':  token_buf_.push_back('\f'); break;
    case 'n':  token_buf_.push_back('\n'); break;
    case 'r':  token_buf_.push_back('\r'); break;
    case 't':  token_buf_.push_back('\t'); break;
    case 'u':
      // Preserve the \uXXXX sequence verbatim in token_buf_ so callers
      // receive the escape rather than a decoded codepoint.  This keeps
      // stop sequences byte-identical to the original request body.
      token_buf_.push_back('\\');
      token_buf_.push_back('u');
      unicode_count_ = 0;
      state_ = ParseState::InStringUnicode;
      return absl::OkStatus();
    default:
      return absl::InvalidArgumentError("invalid escape in string value");
    }
    state_ = ParseState::InStringValue;
    break;

  case ParseState::InStringUnicode:
    if (!std::isxdigit(c))
      return absl::InvalidArgumentError("non-hex digit in \\uXXXX escape");
    token_buf_.push_back(static_cast<char>(c));
    if (++unicode_count_ == 4) {
      unicode_count_ = 0;
      state_ = ParseState::InStringValue;
    }
    break;

  // ── Number ────────────────────────────────────────────────────────────────
  case ParseState::InNumber:
    if (std::isdigit(c) || c == '.' || c == 'e' || c == 'E' ||
        c == '+' || c == '-') {
      token_buf_.push_back(static_cast<char>(c));
    } else {
      auto s = fireNumber();
      if (!s.ok()) return s;
      state_    = ParseState::ExpectCommaOrClose;
      reprocess = true; // re-examine c in ExpectCommaOrClose
    }
    break;

  // ── Keyword: true / false / null ─────────────────────────────────────────
  case ParseState::InKeyword:
    if (std::isalpha(c) && token_buf_.size() < 5) {
      token_buf_.push_back(static_cast<char>(c));
      if (token_buf_ == "true" || token_buf_ == "false" ||
          token_buf_ == "null") {
        auto s = fireKeyword();
        if (!s.ok()) return s;
        state_ = ParseState::ExpectCommaOrClose;
      }
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("invalid keyword token: '", token_buf_, "'"));
    }
    break;

  // ── Capture mode: forward raw bytes, track depth for container close ──────
  case ParseState::InCapture:
    capture_writer_->append({reinterpret_cast<const char*>(&c), 1});
    if (cap_in_escape_) {
      cap_in_escape_ = false;
    } else if (cap_in_string_) {
      if      (c == '\\') cap_in_escape_ = true;
      else if (c == '"')  cap_in_string_ = false;
    } else {
      if      (c == '"')             { cap_in_string_ = true; }
      else if (c == '{' || c == '[') { ++cap_depth_counter_; }
      else if (c == '}' || c == ']') {
        if (--cap_depth_counter_ == 0) {
          capture_writer_ = nullptr;
          --depth_;
          bool ok = (c == '}') ? handler_.onEndObject()
                               : handler_.onEndArray();
          if (!ok)
            return absl::InvalidArgumentError("handler error at capture end");
          state_ = (depth_ == 0) ? ParseState::Done
                                  : ParseState::ExpectCommaOrClose;
        }
      }
    }
    break;

  // ── Done ─────────────────────────────────────────────────────────────────
  case ParseState::Done:
    if (!std::isspace(c))
      return absl::InvalidArgumentError("trailing content after JSON document");
    break;
  }

  return absl::OkStatus();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser
//
// Replaces the old buffer-then-SAX approach with incremental streaming:
//
//   feed(chunk):
//     1. Enforce hard body-size limit (Tier 3).
//     2. Stream chunk bytes to the residual_params StreamWriter (opened lazily
//        on the first call) — so the full body lands in the store without ever
//        being assembled in a contiguous heap buffer.
//     3. Feed the same bytes to IncrementalJsonTokenizer.  The tokenizer fires
//        scalar callbacks immediately; for messages[]/tools[] elements it calls
//        startCapture() to stream element bytes into a per-element StreamWriter.
//
//   finish():
//     Flush any in-progress number token, finalise the residual_params writer,
//     and move scalar extractions into the payload.
//
// Peak memory per tier (body size B, element sizes Eᵢ):
//   Tier 1 (B ≤ max_element_capture): chunk window + Σ active writer buffers
//   Tier 2 (B ≤ max_body):            chunk window only  (no element capture)
//   Tier 3 (B >  max_body):           ≤ max_body_bytes (hard reject)
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser {
public:
  InferenceBodyParser(const DecoderConfig& config, PayloadStore& store)
      : config_(config), store_(store), handler_(*this), tokenizer_(handler_) {}

  absl::Status feed(absl::string_view chunk) {
    total_bytes_ += chunk.size();
    if (total_bytes_ > config_.max_body_bytes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "inference request body exceeds limit of ", config_.max_body_bytes, " bytes"));
    }
    // Open the residual_params writer on the first chunk.
    if (!residual_writer_) {
      residual_writer_ = store_.beginStore(PayloadKind::JsonObject);
    }
    residual_writer_->append(chunk);
    auto s = tokenizer_.feed(chunk);
    if (!s.ok() && handler_.has_error_) {
      return absl::InvalidArgumentError(handler_.error_);
    }
    return s;
  }

  absl::Status finish(InferencePayload& payload, AiRequest& request) {
    auto s = tokenizer_.finish();
    if (!s.ok()) return absl::InvalidArgumentError(
        absl::StrCat("inference body parse error: ", s.message()));

    if (handler_.has_error_) {
      return absl::InvalidArgumentError(
          absl::StrCat("inference body parse error: ", handler_.error_));
    }

    // Move accumulated scalars into the payload.
    payload.target.name       = std::move(handler_.model_);
    payload.sampling          = std::move(handler_.sampling_);
    payload.messages          = std::move(handler_.messages_);
    payload.tools             = std::move(handler_.tools_);
    request.streaming         = handler_.streaming_;

    if (residual_writer_) {
      payload.residual_params = residual_writer_->finalize();
    }
    return absl::OkStatus();
  }

private:
  // ── InferenceHandler ────────────────────────────────────────────────────
  struct InferenceHandler : public IncrementalJsonTokenizer::Handler {
    explicit InferenceHandler(InferenceBodyParser& p) : parser_(p) {}

    // ── accumulated scalars ──
    std::string              model_;
    bool                     streaming_{false};
    SamplingParams           sampling_;
    std::vector<PayloadRef>  messages_;
    std::vector<PayloadRef>  tools_;
    bool                     has_error_{false};
    std::string              error_;

    // ── per-parse state ──
    int         depth_{0};
    std::string current_key_;
    bool        in_messages_{false};
    bool        in_tools_{false};
    bool        in_stop_array_{false};

    // ── duplicate-key tracking (depth=1 extracted fields only) ──
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

    // ── element capture state ──
    bool                         is_capturing_{false};
    int                          capture_depth_{0};
    std::unique_ptr<StreamWriter> elem_writer_;

    InferenceBodyParser& parser_;

    bool captureEnabled() const {
      return parser_.total_bytes_ <= parser_.config_.max_element_capture_bytes;
    }

    bool onStartObject() override {
      ++depth_;
      if (!is_capturing_ && captureEnabled() &&
          depth_ == 3 && (in_messages_ || in_tools_)) {
        is_capturing_ = true;
        capture_depth_ = depth_;
        elem_writer_   = parser_.store_.beginStore(PayloadKind::JsonObject);
        parser_.tokenizer_.startCapture(*elem_writer_);
      }
      return true;
    }

    bool onEndObject() override {
      if (is_capturing_ && depth_ == capture_depth_) {
        is_capturing_ = false;
        PayloadRef ref = elem_writer_->finalize();
        elem_writer_.reset();
        if (in_messages_) messages_.push_back(std::move(ref));
        else              tools_.push_back(std::move(ref));
      }
      --depth_;
      if (depth_ == 1) {
        in_messages_   = false;
        in_tools_      = false;
        in_stop_array_ = false;
      }
      return true;
    }

    bool onStartArray() override {
      ++depth_;
      if (depth_ == 2) {
        if      (current_key_ == "messages") { in_messages_ = true; in_tools_ = false; }
        else if (current_key_ == "tools")    { in_tools_    = true; in_messages_ = false; }
        else if (current_key_ == "stop")     { in_stop_array_ = true; }
      }
      // Array-typed element inside messages/tools (unusual but valid JSON).
      if (!is_capturing_ && captureEnabled() &&
          depth_ == 3 && (in_messages_ || in_tools_)) {
        is_capturing_  = true;
        capture_depth_ = depth_;
        elem_writer_   = parser_.store_.beginStore(PayloadKind::JsonArray);
        parser_.tokenizer_.startCapture(*elem_writer_);
      }
      return true;
    }

    bool onEndArray() override {
      if (is_capturing_ && depth_ == capture_depth_) {
        is_capturing_ = false;
        PayloadRef ref = elem_writer_->finalize();
        elem_writer_.reset();
        if (in_messages_) messages_.push_back(std::move(ref));
        else              tools_.push_back(std::move(ref));
      }
      --depth_;
      if (depth_ == 1) {
        in_messages_   = false;
        in_tools_      = false;
        in_stop_array_ = false;
      }
      return true;
    }

    bool onKey(absl::string_view key) override {
      if (depth_ == 1) {
        // Reject duplicate top-level keys for all extracted fields.
        // Returning false aborts the parse immediately with an error.
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

        if (seen != nullptr) {
          if (*seen) {
            has_error_ = true;
            error_     = absl::StrCat("duplicate key \"", key, "\" in inference request body");
            return false;
          }
          *seen = true;
        }

        current_key_   = key;
        in_stop_array_ = false;
      }
      return true;
    }

    bool onString(absl::string_view v) override {
      if (depth_ == 1 && current_key_ == "model") {
        model_ = std::string(v);
      } else if (depth_ == 1 && current_key_ == "stop") {
        sampling_.stop.push_back(std::string(v)); // stop as a scalar string
      } else if (depth_ == 2 && in_stop_array_) {
        sampling_.stop.push_back(std::string(v)); // stop as an array element
      }
      return true;
    }

    bool onInt(int64_t v) override {
      if (depth_ == 1) {
        if      (current_key_ == "max_tokens") sampling_.max_tokens = static_cast<int32_t>(v);
        else if (current_key_ == "n")          sampling_.n          = static_cast<int32_t>(v);
        else if (current_key_ == "seed")       sampling_.seed       = v;
      }
      return true;
    }

    bool onFloat(double v) override {
      if (depth_ == 1) {
        if      (current_key_ == "temperature") sampling_.temperature = v;
        else if (current_key_ == "top_p")       sampling_.top_p       = v;
      }
      return true;
    }

    bool onBool(bool v) override {
      if (depth_ == 1 && current_key_ == "stream") {
        streaming_ = v;
      }
      return true;
    }

    bool onNull() override { return true; }
  };

  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  size_t                        total_bytes_{0};
  InferenceHandler              handler_;        // must be before tokenizer_
  IncrementalJsonTokenizer      tokenizer_;
  std::unique_ptr<StreamWriter> residual_writer_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser
//
// Same incremental streaming strategy for JSON-RPC 2.0 bodies.
//
// id and method are extracted as scalars at depth=1.  The params value
// (object or array at depth=2) is captured verbatim into a std::string via
// a StringStreamWriter so it can be passed to json::parse() for field
// extraction (tool_name, resource_uri, etc.) in finish().
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path, const DecoderConfig& config, PayloadStore& store)
      : headers_(headers), http_method_(http_method), path_(path),
        config_(config), store_(store), handler_(*this), tokenizer_(handler_) {}

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
    auto s = tokenizer_.feed(chunk);
    if (!s.ok() && handler_.has_error_) {
      return absl::InvalidArgumentError(handler_.error_);
    }
    return s;
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request) {
    auto s = tokenizer_.finish();
    if (!s.ok()) return absl::InvalidArgumentError(
        absl::StrCat("agent body parse error: ", s.message()));

    if (handler_.has_error_) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent body JSON-RPC parse error: ", handler_.error_));
    }

    request.jsonrpc_id = std::move(handler_.id_);
    request.rpc_method = std::move(handler_.method_);

    if (!request.rpc_method.empty()) {
      ClassifyResult r2  = classify({http_method_, path_, headers_, request.rpc_method});
      request.protocol   = r2.protocol;
      if (auto* inv = std::get_if<AgentInvocation>(&r2.invocation)) {
        payload.invocation = *inv;
      }
      payload.dialect = (r2.protocol == ProtocolKind::AgenticMcp) ? AgentDialect::Mcp
                                                                   : AgentDialect::A2a;
    }

    if (handler_.params_captured_) {
      std::string params_json = std::move(handler_.params_buf_);
      const json params = json::parse(params_json, nullptr, /*allow_exceptions=*/false);
      if (!params.is_discarded()) {
        populateParams(params, payload, store_);
      }
      payload.params_raw = store_.store(std::move(params_json), PayloadKind::JsonObject);
    }

    if (residual_writer_) {
      payload.residual_params = residual_writer_->finalize();
    }
    return absl::OkStatus();
  }

private:
  // Minimal StreamWriter that accumulates into a std::string.  Used for params
  // capture so the bytes can be passed to json::parse() in finish().
  struct StringStreamWriter : public StreamWriter {
    std::string buf;
    void       append(absl::string_view bytes) override { buf.append(bytes); }
    PayloadRef finalize() override { return PayloadRef{}; } // unused
  };

  // ── AgentHandler ─────────────────────────────────────────────────────────
  struct AgentHandler : public IncrementalJsonTokenizer::Handler {
    explicit AgentHandler(AgentBodyParser& p) : parser_(p) {}

    std::string id_;
    std::string method_;
    bool        params_captured_{false};
    std::string params_buf_;
    bool        has_error_{false};
    std::string error_;

    int         depth_{0};
    std::string current_key_;

    // ── duplicate-key tracking (depth=1 extracted fields only) ──
    bool seen_jsonrpc_{false};
    bool seen_id_{false};
    bool seen_method_{false};
    bool seen_params_{false};

    bool        capturing_params_{false};
    int         params_capture_depth_{0};
    StringStreamWriter params_writer_;

    AgentBodyParser& parser_;

    bool captureEnabled() const {
      return parser_.total_bytes_ <= parser_.config_.max_element_capture_bytes;
    }

    bool onStartObject() override {
      ++depth_;
      if (!capturing_params_ && captureEnabled() &&
          depth_ == 2 && current_key_ == "params") {
        capturing_params_    = true;
        params_capture_depth_ = depth_;
        parser_.tokenizer_.startCapture(params_writer_);
      }
      return true;
    }

    bool onEndObject() override {
      if (capturing_params_ && depth_ == params_capture_depth_) {
        capturing_params_ = false;
        params_captured_  = true;
        params_buf_       = std::move(params_writer_.buf);
      }
      --depth_;
      return true;
    }

    bool onStartArray() override {
      ++depth_;
      // params may be an array per JSON-RPC spec (positional arguments).
      if (!capturing_params_ && captureEnabled() &&
          depth_ == 2 && current_key_ == "params") {
        capturing_params_    = true;
        params_capture_depth_ = depth_;
        parser_.tokenizer_.startCapture(params_writer_);
      }
      return true;
    }

    bool onEndArray() override {
      if (capturing_params_ && depth_ == params_capture_depth_) {
        capturing_params_ = false;
        params_captured_  = true;
        params_buf_       = std::move(params_writer_.buf);
      }
      --depth_;
      return true;
    }

    bool onKey(absl::string_view key) override {
      if (depth_ == 1) {
        bool* seen = nullptr;
        if      (key == "jsonrpc") seen = &seen_jsonrpc_;
        else if (key == "id")      seen = &seen_id_;
        else if (key == "method")  seen = &seen_method_;
        else if (key == "params")  seen = &seen_params_;

        if (seen != nullptr) {
          if (*seen) {
            has_error_ = true;
            error_     = absl::StrCat("duplicate key \"", key, "\" in agent request body");
            return false;
          }
          *seen = true;
        }

        current_key_ = key;
      }
      return true;
    }

    bool onString(absl::string_view v) override {
      if (depth_ == 1) {
        if      (current_key_ == "id")     id_     = std::string(v);
        else if (current_key_ == "method") method_ = std::string(v);
      }
      return true;
    }

    bool onInt(int64_t v) override {
      if (depth_ == 1 && current_key_ == "id") id_ = std::to_string(v);
      return true;
    }

    bool onFloat(double v) override {
      if (depth_ == 1 && current_key_ == "id")
        id_ = std::to_string(static_cast<int64_t>(v));
      return true;
    }

    bool onBool(bool /*v*/) override { return true; }
    bool onNull()           override { return true; }
  };

  static void populateParams(const json& params, AgentPayload& payload, PayloadStore& store) {
    auto str = [&](const char* k) -> std::string {
      return (params.contains(k) && params[k].is_string()) ? params[k].get<std::string>() : "";
    };
    auto field = [&](const char* k) -> PayloadRef {
      return params.contains(k) ? store.store(params[k].dump(), PayloadKind::JsonObject)
                                : PayloadRef{};
    };
    switch (payload.invocation) {
    case AgentInvocation::ToolsCall:
      payload.tool_name = str("name");
      payload.arguments = field("arguments");
      break;
    case AgentInvocation::ResourcesRead:
    case AgentInvocation::ResourcesSubscribe:
    case AgentInvocation::ResourcesUnsubscribe:
      payload.resource_uri = str("uri");
      break;
    case AgentInvocation::PromptsGet:
      payload.prompt_name = str("name");
      payload.arguments   = field("arguments");
      break;
    case AgentInvocation::CompletionComplete:
      payload.completion_ref = str("ref");
      break;
    case AgentInvocation::Initialize:
      payload.capabilities = field("capabilities");
      break;
    default:
      break;
    }
  }

  const Http::RequestHeaderMap& headers_;
  const std::string             http_method_;
  const std::string             path_;
  const DecoderConfig&          config_;
  PayloadStore&                 store_;
  size_t                        total_bytes_{0};
  AgentHandler                  handler_;        // must be before tokenizer_
  IncrementalJsonTokenizer      tokenizer_;
  std::unique_ptr<StreamWriter> residual_writer_;
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
