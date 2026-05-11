#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

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
// BufferByteIterator
//
// Input iterator that walks bytes across a Buffer::RawSliceVector without
// copying slice data. Passed to nlohmann::json::sax_parse() so the parser
// reads directly from the OwnedImpl slab chain.
//
// Optionally tracks how many bytes have been consumed via a caller-supplied
// size_t counter (pos). The SAX handlers read this counter to record the byte
// positions of element boundaries and slice raw bytes directly from the buffer
// — avoiding re-serialization and the normalization it introduces.
//
// Copy semantics: pos_ is shared across all copies (= default). nlohmann
// copies the begin iterator into its internal input_adapter, so all copies
// must share the same counter or the copy doing the actual advancing will not
// increment it. This is safe because nlohmann is a sequential reader — only
// one copy is ever advancing at a time.
// ─────────────────────────────────────────────────────────────────────────────

class BufferByteIterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type        = char;
  using difference_type   = std::ptrdiff_t;
  using pointer           = const char*;
  using reference         = char;

  // Begin iterator — tracks consumed bytes via *pos.
  BufferByteIterator(const Buffer::RawSliceVector& slices, size_t si, size_t bi, size_t* pos)
      : slices_(&slices), slice_idx_(si), byte_idx_(bi), pos_(pos) {}

  // End iterator — no position tracking needed.
  BufferByteIterator(const Buffer::RawSliceVector& slices, size_t si, size_t bi)
      : slices_(&slices), slice_idx_(si), byte_idx_(bi), pos_(nullptr) {}

  // Default copy/move: pos_ is shared across copies. This is intentional —
  // nlohmann copies the iterator into its internal adapter, so all copies must
  // share the same counter. Safe because nlohmann advances only one copy at a
  // time (sequential reader, no concurrent lookahead copies).
  BufferByteIterator(const BufferByteIterator&)            = default;
  BufferByteIterator(BufferByteIterator&&)                 = default;
  BufferByteIterator& operator=(const BufferByteIterator&) = default;
  BufferByteIterator& operator=(BufferByteIterator&&)      = default;

  char operator*() const {
    return static_cast<const char*>((*slices_)[slice_idx_].mem_)[byte_idx_];
  }

  BufferByteIterator& operator++() {
    if (slice_idx_ >= slices_->size()) {
      return *this;
    }
    if (++byte_idx_ >= (*slices_)[slice_idx_].len_) {
      ++slice_idx_;
      byte_idx_ = 0;
    }
    if (pos_) {
      ++(*pos_);
    }
    return *this;
  }

  bool operator==(const BufferByteIterator& o) const {
    return slice_idx_ == o.slice_idx_ && byte_idx_ == o.byte_idx_;
  }
  bool operator!=(const BufferByteIterator& o) const { return !(*this == o); }

private:
  const Buffer::RawSliceVector* slices_;
  size_t                        slice_idx_;
  size_t                        byte_idx_;
  size_t*                       pos_;
};

// Index of the first non-empty slice (begin iterator seed).
size_t firstNonEmpty(const Buffer::RawSliceVector& slices) {
  for (size_t i = 0; i < slices.size(); ++i) {
    if (slices[i].len_ > 0) {
      return i;
    }
  }
  return slices.size();
}

// Copies bytes [start, end) from a non-contiguous RawSliceVector into a
// std::string. Used to extract element sub-ranges from the body slab chain
// without going through the SAX event re-serialization path.
std::string sliceBuffer(const Buffer::RawSliceVector& slices, size_t start, size_t end) {
  std::string result;
  if (end <= start) {
    return result;
  }
  result.reserve(end - start);
  size_t offset = 0;
  for (const auto& slice : slices) {
    const size_t slice_end = offset + slice.len_;
    if (slice_end <= start) {
      offset = slice_end;
      continue;
    }
    if (offset >= end) {
      break;
    }
    const size_t copy_from = std::max(start, offset) - offset;
    const size_t copy_to   = std::min(end, slice_end) - offset;
    result.append(static_cast<const char*>(slice.mem_) + copy_from, copy_to - copy_from);
    offset = slice_end;
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// InferenceSAXHandler
//
// SAX handler for OpenAI-style REST bodies. Extracts scalar fields (model,
// stream, sampling params) inline as SAX events fire.
//
// For messages[] and tools[] elements, uses byte-range capture: records the
// buffer position when an element's opening { or [ is consumed, then slices
// the raw bytes [start, end) from the slab chain when the matching close fires.
// No DOM is constructed and no re-serialization is performed — the bytes stored
// in the PayloadRef are identical to what appeared in the original request body.
//
// This is strictly better than the previous SubtreeSerializer approach:
//   - No re-escaping of strings (unicode sequences preserved exactly)
//   - No character-by-character scan of large content strings
//   - Events inside a captured element are entirely ignored (no per-event work)
//   - Element capture is a simple depth counter + two memcpy calls
// ─────────────────────────────────────────────────────────────────────────────

struct InferenceSAXHandler : public nlohmann::json_sax<json> {
  using string_t          = json::string_t;
  using number_integer_t  = json::number_integer_t;
  using number_unsigned_t = json::number_unsigned_t;
  using number_float_t    = json::number_float_t;
  using binary_t          = json::binary_t;

  InferencePayload&             payload_;
  AiRequest&                    request_;
  PayloadStore&                 store_;
  const size_t*                 parser_pos_;  // bytes consumed by nlohmann so far
  const Buffer::RawSliceVector* slices_;      // body slab chain for byte slicing

  int         depth_{0};
  std::string current_key_;

  bool in_messages_{false};
  bool in_tools_{false};

  // Byte-range capture state.
  bool   capturing_element_{false};
  size_t elem_start_{0};   // position of element's opening { or [
  int    elem_depth_{0};   // nesting depth inside the captured element

  bool        has_error_{false};
  std::string error_;

  InferenceSAXHandler(InferencePayload& p, AiRequest& r, PayloadStore& s,
                      const size_t* pos, const Buffer::RawSliceVector* slices)
      : payload_(p), request_(r), store_(s), parser_pos_(pos), slices_(slices) {}

  // Scalars inside a captured element are ignored — byte-range capture
  // records the raw bytes, so we don't need to process individual values.

  bool null() override { return true; }

  bool boolean(bool v) override {
    if (capturing_element_) return true;
    if (depth_ == 1 && current_key_ == "stream") {
      request_.streaming = v;
    }
    return true;
  }

  bool number_integer(number_integer_t v) override {
    if (capturing_element_) return true;
    if (depth_ == 1) {
      if      (current_key_ == "max_tokens") payload_.sampling.max_tokens = static_cast<int32_t>(v);
      else if (current_key_ == "n")          payload_.sampling.n          = static_cast<int32_t>(v);
      else if (current_key_ == "seed")       payload_.sampling.seed       = v;
    }
    return true;
  }

  bool number_unsigned(number_unsigned_t v) override {
    if (capturing_element_) return true;
    if (depth_ == 1) {
      if      (current_key_ == "max_tokens") payload_.sampling.max_tokens = static_cast<int32_t>(v);
      else if (current_key_ == "n")          payload_.sampling.n          = static_cast<int32_t>(v);
      else if (current_key_ == "seed")       payload_.sampling.seed       = static_cast<int64_t>(v);
    }
    return true;
  }

  bool number_float(number_float_t v, const string_t& /*raw*/) override {
    if (capturing_element_) return true;
    if (depth_ == 1) {
      if      (current_key_ == "temperature") payload_.sampling.temperature = v;
      else if (current_key_ == "top_p")       payload_.sampling.top_p       = v;
    }
    return true;
  }

  bool string(string_t& v) override {
    if (capturing_element_) return true;
    if (depth_ == 1 && current_key_ == "model") {
      payload_.target.name = v;
    } else if (current_key_ == "stop" && (depth_ == 1 || depth_ == 2)) {
      payload_.sampling.stop.push_back(v);
    }
    return true;
  }

  bool binary(binary_t& /*v*/) override { return true; }

  bool start_object(std::size_t) override {
    if (capturing_element_) {
      ++elem_depth_;
      return true;
    }
    ++depth_;
    if (depth_ == 3 && (in_messages_ || in_tools_)) {
      capturing_element_ = true;
      elem_depth_        = 1;
      elem_start_        = *parser_pos_ - 1;  // { was the last byte consumed
    }
    return true;
  }

  bool key(string_t& k) override {
    if (capturing_element_) return true;
    if (depth_ == 1) {
      current_key_ = k;
    }
    return true;
  }

  bool end_object() override {
    if (capturing_element_) {
      if (--elem_depth_ == 0) {
        storeElement();
        --depth_;  // depth_ was 3 (element level) → 2 (array level)
      }
      return true;
    }
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (capturing_element_) {
      ++elem_depth_;
      return true;
    }
    ++depth_;
    if (depth_ == 2) {
      if      (current_key_ == "messages") { in_messages_ = true;  in_tools_ = false; }
      else if (current_key_ == "tools")    { in_tools_    = true; in_messages_ = false; }
    } else if (depth_ == 3 && (in_messages_ || in_tools_)) {
      // Array-typed element inside messages/tools (unusual but valid JSON).
      capturing_element_ = true;
      elem_depth_        = 1;
      elem_start_        = *parser_pos_ - 1;
    }
    return true;
  }

  bool end_array() override {
    if (capturing_element_) {
      if (--elem_depth_ == 0) {
        storeElement();
        --depth_;
      }
      return true;
    }
    --depth_;
    if (depth_ == 1) {
      in_messages_ = false;
      in_tools_    = false;
    }
    return true;
  }

  bool parse_error(std::size_t pos, const std::string& /*token*/,
                   const nlohmann::detail::exception& ex) override {
    has_error_ = true;
    error_     = absl::StrCat("JSON parse error at byte ", pos, ": ", ex.what());
    return false;
  }

private:
  void storeElement() {
    // Slice the raw bytes of this element directly from the body slab chain.
    // parser_pos_ is one past the closing } or ] at the moment this fires.
    std::string raw = sliceBuffer(*slices_, elem_start_, *parser_pos_);
    capturing_element_ = false;
    PayloadRef ref = store_.store(std::move(raw), PayloadKind::JsonObject);
    if (in_messages_) {
      payload_.messages.push_back(std::move(ref));
    } else {
      payload_.tools.push_back(std::move(ref));
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentSAXHandler
//
// SAX handler for JSON-RPC 2.0 agent bodies. Extracts the envelope (id,
// method) inline; captures the params value via byte-range capture — records
// the position of the params opening { or [ and slices raw bytes when the
// matching close fires. No DOM is held during the SAX pass.
//
// AgentBodyParser::finish does one json::parse of the captured params string
// for field extraction (tool_name, resource_uri, etc.), then moves the exact-
// byte string into the store for params_raw.
// ─────────────────────────────────────────────────────────────────────────────

struct AgentSAXHandler : public nlohmann::json_sax<json> {
  using string_t          = json::string_t;
  using number_integer_t  = json::number_integer_t;
  using number_unsigned_t = json::number_unsigned_t;
  using number_float_t    = json::number_float_t;
  using binary_t          = json::binary_t;

  AiRequest&                    request_;
  const size_t*                 parser_pos_;
  const Buffer::RawSliceVector* slices_;

  int         depth_{0};
  std::string current_key_;

  bool   capturing_params_{false};
  size_t params_start_{0};
  int    params_depth_{0};
  bool   params_captured_{false};
  std::string params_json_;

  bool        has_error_{false};
  std::string error_;

  AgentSAXHandler(AiRequest& r, const size_t* pos, const Buffer::RawSliceVector* slices)
      : request_(r), parser_pos_(pos), slices_(slices) {}

  bool null() override { return true; }

  bool boolean(bool /*v*/) override { return true; }

  bool number_integer(number_integer_t v) override {
    if (capturing_params_) return true;
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(v);
    }
    return true;
  }

  bool number_unsigned(number_unsigned_t v) override {
    if (capturing_params_) return true;
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(v);
    }
    return true;
  }

  bool number_float(number_float_t v, const string_t& /*raw*/) override {
    if (capturing_params_) return true;
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(static_cast<int64_t>(v));
    }
    return true;
  }

  bool string(string_t& v) override {
    if (capturing_params_) return true;
    if (depth_ == 1) {
      if      (current_key_ == "id")     request_.jsonrpc_id = v;
      else if (current_key_ == "method") request_.rpc_method = v;
    }
    return true;
  }

  bool binary(binary_t& /*v*/) override { return true; }

  bool start_object(std::size_t) override {
    if (capturing_params_) {
      ++params_depth_;
      return true;
    }
    ++depth_;
    if (depth_ == 2 && current_key_ == "params") {
      capturing_params_ = true;
      params_depth_     = 1;
      params_start_     = *parser_pos_ - 1;
    }
    return true;
  }

  bool key(string_t& k) override {
    if (capturing_params_) return true;
    if (depth_ == 1) {
      current_key_ = k;
    }
    return true;
  }

  bool end_object() override {
    if (capturing_params_) {
      if (--params_depth_ == 0) captureParams();
      return true;
    }
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (capturing_params_) {
      ++params_depth_;
      return true;
    }
    ++depth_;
    // params may be an array per JSON-RPC spec (positional arguments).
    if (depth_ == 2 && current_key_ == "params") {
      capturing_params_ = true;
      params_depth_     = 1;
      params_start_     = *parser_pos_ - 1;
    }
    return true;
  }

  bool end_array() override {
    if (capturing_params_) {
      if (--params_depth_ == 0) captureParams();
      return true;
    }
    --depth_;
    return true;
  }

  bool parse_error(std::size_t pos, const std::string& /*token*/,
                   const nlohmann::detail::exception& ex) override {
    has_error_ = true;
    error_     = absl::StrCat("JSON-RPC parse error at byte ", pos, ": ", ex.what());
    return false;
  }

  bool hasParams() const { return params_captured_; }
  std::string takeParamsJson() { return std::move(params_json_); }

private:
  void captureParams() {
    params_json_      = sliceBuffer(*slices_, params_start_, *parser_pos_);
    params_captured_  = true;
    capturing_params_ = false;
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser
//
// Accumulates the full OpenAI-style REST JSON body in a Buffer::OwnedImpl
// (Envoy's iovec slab allocator) rather than a std::string.
//
// At onEndStream, runs a SAX parse pass directly over the buffer's
// RawSliceVector via BufferByteIterator — no contiguous copy. Scalar fields
// are extracted inline; each messages[] and tools[] element is captured by
// slicing raw bytes directly from the slab chain (no DOM, no re-serialization,
// byte-identical to the original request body).
//
// residual_params receives the entire body buffer via store(Buffer::Instance&),
// which does a zero-copy slab transfer (Buffer::move) for payloads above the
// inline threshold.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser {
public:
  void feed(absl::string_view chunk) {
    body_buffer_.add(chunk.data(), chunk.size());
  }

  absl::Status finish(InferencePayload& payload, AiRequest& request, PayloadStore& store,
                      const DecoderConfig& config) {
    if (body_buffer_.length() == 0) {
      return absl::OkStatus();
    }

    Buffer::RawSliceVector slices = body_buffer_.getRawSlices();
    const size_t           first  = firstNonEmpty(slices);

    // pos tracks bytes consumed by the parser; handlers read it to locate
    // element boundaries and slice raw bytes via sliceBuffer().
    size_t pos = 0;
    BufferByteIterator begin(slices, first, 0, &pos);
    BufferByteIterator end(slices, slices.size(), 0);

    InferenceSAXHandler handler(payload, request, store, &pos, &slices);
    const bool ok = nlohmann::json::sax_parse(
        std::move(begin), std::move(end), &handler,
        nlohmann::detail::input_format_t::json, /*strict=*/false);

    if (!ok || handler.has_error_) {
      return absl::InvalidArgumentError(absl::StrCat(
          "inference body JSON parse error: ",
          handler.has_error_ ? handler.error_ : "invalid JSON"));
    }

    payload.residual_params = store.store(body_buffer_, PayloadKind::JsonObject);

    (void)config;
    return absl::OkStatus();
  }

private:
  Buffer::OwnedImpl body_buffer_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser
//
// Same external-buffer + SAX strategy for JSON-RPC 2.0 agent bodies.
// AgentSAXHandler extracts id and method inline; params is captured as raw
// bytes via byte-range slicing — no DOM during the SAX pass. AgentBodyParser::
// finish does one json::parse of the exact-byte params string for field
// extraction, then moves the string into the store for params_raw.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path)
      : headers_(headers), http_method_(http_method), path_(path) {}

  absl::Status feed(absl::string_view chunk) {
    body_buffer_.add(chunk.data(), chunk.size());
    return absl::OkStatus();
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request, PayloadStore& store) {
    if (body_buffer_.length() == 0) {
      return absl::OkStatus();
    }

    Buffer::RawSliceVector slices = body_buffer_.getRawSlices();
    const size_t           first  = firstNonEmpty(slices);

    size_t pos = 0;
    BufferByteIterator begin(slices, first, 0, &pos);
    BufferByteIterator end(slices, slices.size(), 0);

    AgentSAXHandler handler(request, &pos, &slices);
    const bool ok = nlohmann::json::sax_parse(
        std::move(begin), std::move(end), &handler,
        nlohmann::detail::input_format_t::json, /*strict=*/false);

    if (!ok || handler.has_error_) {
      return absl::InvalidArgumentError(absl::StrCat(
          "agent body JSON-RPC parse error: ",
          handler.has_error_ ? handler.error_ : "invalid JSON"));
    }

    if (!request.rpc_method.empty()) {
      ClassifyResult r2  = classify({http_method_, path_, headers_, request.rpc_method});
      request.protocol   = r2.protocol;
      if (auto* inv = std::get_if<AgentInvocation>(&r2.invocation)) {
        payload.invocation = *inv;
      }
      payload.dialect = (r2.protocol == ProtocolKind::AgenticMcp) ? AgentDialect::Mcp
                                                                   : AgentDialect::A2a;
    }

    if (handler.hasParams()) {
      // Parse the exact-byte params string for field extraction, then move it
      // into the store. One DOM parse, no extra copy, bytes identical to input.
      std::string params_json = handler.takeParamsJson();
      const json params = json::parse(params_json, nullptr, /*allow_exceptions=*/false);
      if (!params.is_discarded()) {
        populateParams(params, payload, store);
      }
      payload.params_raw = store.store(std::move(params_json), PayloadKind::JsonObject);
    }

    payload.residual_params = store.store(body_buffer_, PayloadKind::JsonObject);

    return absl::OkStatus();
  }

private:
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
  Buffer::OwnedImpl             body_buffer_;
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
    inference_parser_ = std::make_unique<InferenceBodyParser>();
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
                                                         request_.path);
    state_           = DecodeState::ParsingAgentBody;
  }

  return absl::OkStatus();
}

absl::Status RequestDecoder::onData(absl::string_view chunk) {
  switch (state_) {
  case DecodeState::ParsingInferenceBody:
    ASSERT(inference_parser_ != nullptr);
    inference_parser_->feed(chunk);
    return absl::OkStatus();

  case DecodeState::ParsingAgentBody:
    ASSERT(agent_parser_ != nullptr);
    return agent_parser_->feed(chunk);

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
    auto status = inference_parser_->finish(*payload, request_, payload_store_, config_);
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
    auto status = agent_parser_->finish(*payload, request_, payload_store_);
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
