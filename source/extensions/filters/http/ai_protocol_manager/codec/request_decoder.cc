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
// ─────────────────────────────────────────────────────────────────────────────

class BufferByteIterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type        = char;
  using difference_type   = std::ptrdiff_t;
  using pointer           = const char*;
  using reference         = char;

  BufferByteIterator(const Buffer::RawSliceVector& slices, size_t si, size_t bi)
      : slices_(&slices), slice_idx_(si), byte_idx_(bi) {}

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

// ─────────────────────────────────────────────────────────────────────────────
// SubtreeBuilder
//
// Reconstructs a nlohmann::json value from SAX events. Used to capture
// sub-documents (messages[] elements, tools[] elements, params object) while
// the main SAX handler continues streaming the outer document.
// Only one sub-tree at a time is live in memory.
// ─────────────────────────────────────────────────────────────────────────────

class SubtreeBuilder {
public:
  void onNull()                { push(nullptr); }
  void onBool(bool v)          { push(v); }
  void onInt(int64_t v)        { push(v); }
  void onUint(uint64_t v)      { push(v); }
  void onFloat(double v)       { push(v); }
  void onString(std::string v) { push(std::move(v)); }

  void onKey(std::string k) {
    ASSERT(!stack_.empty() && stack_.back().value.is_object());
    stack_.back().pending_key = std::move(k);
  }

  void onStartObject() { stack_.push_back({json::object(), ""}); }
  void onEndObject()   { finishContainer(); }
  void onStartArray()  { stack_.push_back({json::array(), ""}); }
  void onEndArray()    { finishContainer(); }

  bool complete() const { return result_complete_; }

  json takeResult() {
    result_complete_ = false;
    return std::move(result_);
  }

private:
  struct Frame {
    json        value;
    std::string pending_key;
  };

  std::vector<Frame> stack_;
  json               result_;
  bool               result_complete_{false};

  void push(json v) {
    if (stack_.empty()) {
      result_          = std::move(v);
      result_complete_ = true;
      return;
    }
    auto& f = stack_.back();
    if (f.value.is_array()) {
      f.value.push_back(std::move(v));
    } else {
      f.value[f.pending_key] = std::move(v);
    }
  }

  void finishContainer() {
    json v = std::move(stack_.back().value);
    stack_.pop_back();
    push(std::move(v));
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// InferenceSAXHandler
//
// SAX handler for OpenAI-style REST bodies. Extracts scalar fields (model,
// stream, sampling params) inline as SAX events fire; captures each
// messages[] and tools[] element through a SubtreeBuilder that is alive only
// for the duration of that one element — never the full document DOM.
// ─────────────────────────────────────────────────────────────────────────────

struct InferenceSAXHandler : public nlohmann::json_sax<json> {
  using string_t          = json::string_t;
  using number_integer_t  = json::number_integer_t;
  using number_unsigned_t = json::number_unsigned_t;
  using number_float_t    = json::number_float_t;
  using binary_t          = json::binary_t;

  InferencePayload& payload_;
  AiRequest&        request_;
  PayloadStore&     store_;

  // Current nesting depth in the root document (not counting sub-builder depth).
  // 0 = before/after root object, 1 = inside root, 2 = inside top-level array/value, …
  int         depth_{0};
  std::string current_key_;  // current key at depth 1; preserved across deeper values

  bool in_messages_{false};  // inside top-level "messages" array
  bool in_tools_{false};     // inside top-level "tools" array

  // Active element capture (alive only within one messages/tools element).
  std::unique_ptr<SubtreeBuilder> elem_builder_;
  int                             elem_depth_{0};  // extra nesting depth within element

  bool        has_error_{false};
  std::string error_;

  InferenceSAXHandler(InferencePayload& p, AiRequest& r, PayloadStore& s)
      : payload_(p), request_(r), store_(s) {}

  bool null() override {
    if (elem_builder_) {
      elem_builder_->onNull();
    }
    return true;
  }

  bool boolean(bool v) override {
    if (elem_builder_) {
      elem_builder_->onBool(v);
      return true;
    }
    if (depth_ == 1 && current_key_ == "stream") {
      request_.streaming = v;
    }
    return true;
  }

  bool number_integer(number_integer_t v) override {
    if (elem_builder_) {
      elem_builder_->onInt(v);
      return true;
    }
    if (depth_ == 1) {
      if      (current_key_ == "max_tokens") payload_.sampling.max_tokens = static_cast<int32_t>(v);
      else if (current_key_ == "n")          payload_.sampling.n          = static_cast<int32_t>(v);
      else if (current_key_ == "seed")       payload_.sampling.seed       = v;
    }
    return true;
  }

  bool number_unsigned(number_unsigned_t v) override {
    if (elem_builder_) {
      elem_builder_->onUint(v);
      return true;
    }
    if (depth_ == 1) {
      if      (current_key_ == "max_tokens") payload_.sampling.max_tokens = static_cast<int32_t>(v);
      else if (current_key_ == "n")          payload_.sampling.n          = static_cast<int32_t>(v);
      else if (current_key_ == "seed")       payload_.sampling.seed       = static_cast<int64_t>(v);
    }
    return true;
  }

  bool number_float(number_float_t v, const string_t&) override {
    if (elem_builder_) {
      elem_builder_->onFloat(v);
      return true;
    }
    if (depth_ == 1) {
      if      (current_key_ == "temperature") payload_.sampling.temperature = v;
      else if (current_key_ == "top_p")       payload_.sampling.top_p       = v;
    }
    return true;
  }

  bool string(string_t& v) override {
    if (elem_builder_) {
      elem_builder_->onString(v);
      return true;
    }
    if (depth_ == 1 && current_key_ == "model") {
      payload_.target.name = v;
    } else if (current_key_ == "stop" && (depth_ == 1 || depth_ == 2)) {
      // "stop" can be a bare string (depth 1) or an array element (depth 2).
      payload_.sampling.stop.push_back(v);
    }
    return true;
  }

  bool binary(binary_t& /*v*/) override { return true; }

  bool start_object(std::size_t) override {
    if (elem_builder_) {
      elem_builder_->onStartObject();
      ++elem_depth_;
      return true;
    }
    ++depth_;
    // depth_ == 3 means we just opened an object that is an element of the
    // messages[] or tools[] array (which lives at depth 2).
    if (depth_ == 3 && (in_messages_ || in_tools_)) {
      elem_builder_ = std::make_unique<SubtreeBuilder>();
      elem_depth_   = 0;
      elem_builder_->onStartObject();
    }
    return true;
  }

  bool key(string_t& k) override {
    if (elem_builder_) {
      elem_builder_->onKey(k);
      return true;
    }
    if (depth_ == 1) {
      current_key_ = k;
    }
    return true;
  }

  bool end_object() override {
    if (elem_builder_) {
      if (elem_depth_ > 0) {
        elem_builder_->onEndObject();
        --elem_depth_;
        return true;
      }
      // Element root is closing: finish capture and return to array depth.
      elem_builder_->onEndObject();
      storeElement();
      --depth_;  // depth_ was 3 (element level) → 2 (array level)
      return true;
    }
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (elem_builder_) {
      elem_builder_->onStartArray();
      ++elem_depth_;
      return true;
    }
    ++depth_;
    if (depth_ == 2) {
      if      (current_key_ == "messages") { in_messages_ = true;  in_tools_ = false; }
      else if (current_key_ == "tools")    { in_tools_    = true; in_messages_ = false; }
    } else if (depth_ == 3 && (in_messages_ || in_tools_)) {
      // Defensive: array-typed element inside messages/tools (unusual but valid JSON).
      elem_builder_ = std::make_unique<SubtreeBuilder>();
      elem_depth_   = 0;
      elem_builder_->onStartArray();
    }
    return true;
  }

  bool end_array() override {
    if (elem_builder_) {
      if (elem_depth_ > 0) {
        elem_builder_->onEndArray();
        --elem_depth_;
        return true;
      }
      elem_builder_->onEndArray();
      storeElement();
      --depth_;  // depth_ was 3 (element level) → 2 (array level)
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
    json      elem = elem_builder_->takeResult();
    elem_builder_.reset();
    elem_depth_ = 0;
    PayloadRef ref = store_.store(elem.dump(), PayloadKind::JsonObject);
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
// method) inline; captures the params value in a SubtreeBuilder so it can
// be stored as a PayloadRef without holding the full body DOM.
// ─────────────────────────────────────────────────────────────────────────────

struct AgentSAXHandler : public nlohmann::json_sax<json> {
  using string_t          = json::string_t;
  using number_integer_t  = json::number_integer_t;
  using number_unsigned_t = json::number_unsigned_t;
  using number_float_t    = json::number_float_t;
  using binary_t          = json::binary_t;

  AiRequest& request_;

  int         depth_{0};
  std::string current_key_;

  std::unique_ptr<SubtreeBuilder> params_builder_;
  int                             params_depth_{0};
  json                            params_value_;
  bool                            params_captured_{false};

  bool        has_error_{false};
  std::string error_;

  explicit AgentSAXHandler(AiRequest& r) : request_(r) {}

  bool null() override {
    if (params_builder_) {
      params_builder_->onNull();
      checkParamsDone();
    }
    return true;
  }

  bool boolean(bool v) override {
    if (params_builder_) {
      params_builder_->onBool(v);
      checkParamsDone();
      return true;
    }
    return true;
  }

  bool number_integer(number_integer_t v) override {
    if (params_builder_) {
      params_builder_->onInt(v);
      checkParamsDone();
      return true;
    }
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(v);
    }
    return true;
  }

  bool number_unsigned(number_unsigned_t v) override {
    if (params_builder_) {
      params_builder_->onUint(v);
      checkParamsDone();
      return true;
    }
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(v);
    }
    return true;
  }

  bool number_float(number_float_t v, const string_t&) override {
    if (params_builder_) {
      params_builder_->onFloat(v);
      checkParamsDone();
      return true;
    }
    if (depth_ == 1 && current_key_ == "id") {
      request_.jsonrpc_id = std::to_string(static_cast<int64_t>(v));
    }
    return true;
  }

  bool string(string_t& v) override {
    if (params_builder_) {
      params_builder_->onString(v);
      checkParamsDone();
      return true;
    }
    if (depth_ == 1) {
      if      (current_key_ == "id")      request_.jsonrpc_id      = v;
      else if (current_key_ == "method")  request_.rpc_method      = v;
      else if (current_key_ == "jsonrpc") request_.jsonrpc_version = v;
    }
    return true;
  }

  bool binary(binary_t& /*v*/) override { return true; }

  bool start_object(std::size_t) override {
    if (params_builder_) {
      params_builder_->onStartObject();
      ++params_depth_;
      return true;
    }
    ++depth_;
    if (depth_ == 2 && current_key_ == "params") {
      params_builder_ = std::make_unique<SubtreeBuilder>();
      params_depth_   = 0;
      params_builder_->onStartObject();
    }
    return true;
  }

  bool key(string_t& k) override {
    if (params_builder_) {
      params_builder_->onKey(k);
      return true;
    }
    if (depth_ == 1) {
      current_key_ = k;
    }
    return true;
  }

  bool end_object() override {
    if (params_builder_) {
      if (params_depth_ > 0) {
        params_builder_->onEndObject();
        --params_depth_;
        return true;
      }
      params_builder_->onEndObject();
      captureParams();
      return true;
    }
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (params_builder_) {
      params_builder_->onStartArray();
      ++params_depth_;
      return true;
    }
    ++depth_;
    // params may be an array per JSON-RPC spec (positional arguments).
    if (depth_ == 2 && current_key_ == "params") {
      params_builder_ = std::make_unique<SubtreeBuilder>();
      params_depth_   = 0;
      params_builder_->onStartArray();
    }
    return true;
  }

  bool end_array() override {
    if (params_builder_) {
      if (params_depth_ > 0) {
        params_builder_->onEndArray();
        --params_depth_;
        return true;
      }
      params_builder_->onEndArray();
      captureParams();
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
  const json& params() const { return params_value_; }

private:
  void checkParamsDone() {
    if (params_builder_ && params_builder_->complete()) {
      captureParams();
    }
  }

  void captureParams() {
    params_value_    = params_builder_->takeResult();
    params_captured_ = true;
    params_builder_.reset();
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
// are extracted inline; each messages[] and tools[] element is captured via
// SubtreeBuilder and stored as an individual PayloadRef (one at a time in
// memory, never the full array).
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

    // Obtain slice descriptors — these are pointers into the slab memory, no copy.
    Buffer::RawSliceVector slices = body_buffer_.getRawSlices();
    const size_t           first  = firstNonEmpty(slices);
    BufferByteIterator     begin(slices, first, 0);
    BufferByteIterator     end(slices, slices.size(), 0);

    InferenceSAXHandler handler(payload, request, store);
    const bool ok = nlohmann::json::sax_parse(
        begin, end, &handler, nlohmann::detail::input_format_t::json, /*strict=*/false);

    if (!ok || handler.has_error_) {
      return absl::InvalidArgumentError(absl::StrCat(
          "inference body JSON parse error: ",
          handler.has_error_ ? handler.error_ : "invalid JSON"));
    }

    // Move the body buffer into residual_params. For payloads above the inline
    // threshold this is a zero-copy slab transfer (Buffer::OwnedImpl::move).
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
// The AgentSAXHandler extracts id and method inline; the params value is
// captured in a SubtreeBuilder. Invocation-specific fields (tool_name,
// resource_uri, …) are extracted from the captured params JSON after the
// rpc_method is known and the second classify() call runs.
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
    BufferByteIterator     begin(slices, first, 0);
    BufferByteIterator     end(slices, slices.size(), 0);

    AgentSAXHandler handler(request);
    const bool ok = nlohmann::json::sax_parse(
        begin, end, &handler, nlohmann::detail::input_format_t::json, /*strict=*/false);

    if (!ok || handler.has_error_) {
      return absl::InvalidArgumentError(absl::StrCat(
          "agent body JSON-RPC parse error: ",
          handler.has_error_ ? handler.error_ : "invalid JSON"));
    }

    // Re-classify with the now-known rpc_method to determine AgentInvocation.
    if (!request.rpc_method.empty()) {
      ClassifyResult r2  = classify({http_method_, path_, headers_, request.rpc_method});
      request.protocol   = r2.protocol;
      if (auto* inv = std::get_if<AgentInvocation>(&r2.invocation)) {
        payload.invocation = *inv;
      }
      payload.dialect = (r2.protocol == ProtocolKind::AgenticMcp) ? AgentDialect::Mcp
                                                                   : AgentDialect::A2a;
    }

    // Extract invocation-specific params fields from the captured sub-tree.
    if (handler.hasParams()) {
      const json& params = handler.params();
      populateParams(params, payload, store);
      payload.params_raw = store.store(params.dump(), PayloadKind::JsonObject);
    }

    // Full body as residual — zero-copy slab transfer for large payloads.
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

    // Extract W3C trace context from params._meta (present on any invocation).
    if (params.contains("_meta") && params["_meta"].is_object()) {
      const auto& meta = params["_meta"];
      auto metaStr = [&](const char* k) -> std::string {
        return (meta.contains(k) && meta[k].is_string()) ? meta[k].get<std::string>() : "";
      };
      payload.meta_traceparent = metaStr("traceparent");
      payload.meta_tracestate  = metaStr("tracestate");
      payload.meta_baggage     = metaStr("baggage");
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
      "",  // rpc_method — body not yet available
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
