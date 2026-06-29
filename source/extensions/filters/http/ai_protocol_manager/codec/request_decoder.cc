#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
// simdjson uses C-style casts inside ARM NEON intrinsic macros (arm_neon.h).
// Suppress the warning precisely around the include rather than for the whole TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include "simdjson.h"
#pragma GCC diagnostic pop

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser
//
// Accumulates the full OpenAI-style REST JSON body in a Buffer::OwnedImpl
// and parses it at onEndStream using simdjson on-demand.
//
// simdjson on-demand is truly lazy: values you do not access are never lexed,
// so skipping a 10 MB base64 image string in a large message element costs
// zero allocation. For elements we do capture, raw_json() returns a
// string_view into the padded buffer — one copy into the PayloadStore.
//
// Three-tier memory strategy (enforced in feed() / finish()):
//   Tier 1 (body ≤ max_element_capture_bytes): full element capture.
//   Tier 2 (body ≤ max_body_bytes):            scalars only; elements skipped.
//   Tier 3 (body > max_body_bytes):            ResourceExhausted from feed().
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser {
public:
  explicit InferenceBodyParser(const DecoderConfig& config) : config_(config) {}

  absl::Status feed(absl::string_view chunk) {
    if (body_buffer_.length() + chunk.size() > config_.max_body_bytes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "inference request body exceeds limit of ", config_.max_body_bytes, " bytes"));
    }
    body_buffer_.add(chunk.data(), chunk.size());
    return absl::OkStatus();
  }

  absl::Status finish(InferencePayload& payload, AiRequest& request, PayloadStore& store) {
    if (body_buffer_.length() == 0) {
      return absl::OkStatus();
    }

    const bool capture_elements = body_buffer_.length() <= config_.max_element_capture_bytes;

    // Copy the slab chain directly into a single allocation that already has
    // SIMDJSON_PADDING zeros appended. This is one copy instead of two
    // (body_buffer_.toString() + padded_string would copy twice). The tail is
    // zero-initialised by the () on new[], satisfying simdjson's SIMD overread
    // requirement without a separate memset.
    const size_t body_size = body_buffer_.length();
    std::unique_ptr<char[]> buf(new char[body_size + simdjson::SIMDJSON_PADDING]());
    {
      Buffer::RawSliceVector slices = body_buffer_.getRawSlices();
      size_t offset = 0;
      for (const auto& slice : slices) {
        std::memcpy(buf.get() + offset, static_cast<const char*>(slice.mem_), slice.len_);
        offset += slice.len_;
      }
    }
    simdjson::padded_string_view padded(buf.get(), body_size,
                                        body_size + simdjson::SIMDJSON_PADDING);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (auto err = parser.iterate(padded).get(doc); err) {
      return absl::InvalidArgumentError(absl::StrCat(
          "inference body JSON parse error: ", simdjson::error_message(err)));
    }

    simdjson::ondemand::object obj;
    if (auto err = doc.get_object().get(obj); err) {
      return absl::InvalidArgumentError("inference body: expected JSON object at top level");
    }

    for (auto field : obj) {
      std::string_view key;
      if (field.unescaped_key().get(key)) continue;

      if (key == "model") {
        std::string_view v;
        if (!field.value().get_string().get(v)) payload.target.name = std::string(v);
      } else if (key == "stream") {
        bool v;
        if (!field.value().get_bool().get(v)) request.streaming = v;
      } else if (key == "max_tokens") {
        int64_t v;
        if (!field.value().get_int64().get(v)) payload.sampling.max_tokens = static_cast<int32_t>(v);
      } else if (key == "n") {
        int64_t v;
        if (!field.value().get_int64().get(v)) payload.sampling.n = static_cast<int32_t>(v);
      } else if (key == "seed") {
        int64_t v;
        if (!field.value().get_int64().get(v)) payload.sampling.seed = v;
      } else if (key == "temperature") {
        double v;
        if (!field.value().get_double().get(v)) payload.sampling.temperature = v;
      } else if (key == "top_p") {
        double v;
        if (!field.value().get_double().get(v)) payload.sampling.top_p = v;
      } else if (key == "stop") {
        auto val = field.value();
        simdjson::ondemand::json_type type;
        if (!val.type().get(type)) {
          if (type == simdjson::ondemand::json_type::string) {
            std::string_view sv;
            if (!val.get_string().get(sv)) payload.sampling.stop.push_back(std::string(sv));
          } else if (type == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array arr;
            if (!val.get_array().get(arr)) {
              for (auto elem : arr) {
                std::string_view sv;
                if (!elem.get_string().get(sv)) payload.sampling.stop.push_back(std::string(sv));
              }
            }
          }
        }
      } else if (key == "messages" || key == "tools") {
        if (!capture_elements) continue;  // simdjson auto-skips the array; zero allocation
        simdjson::ondemand::array arr;
        if (field.value().get_array().get(arr)) continue;
        const bool is_messages = (key == "messages");
        for (auto elem : arr) {
          std::string_view raw;
          if (elem.raw_json().get(raw)) continue;
          PayloadRef ref = store.store(std::string(raw), PayloadKind::JsonObject);
          if (is_messages) payload.messages.push_back(std::move(ref));
          else             payload.tools.push_back(std::move(ref));
        }
      }
      // All other fields: simdjson automatically skips the value when the
      // iterator advances — no allocation for unrecognised fields.
    }

    payload.residual_params = store.store(body_buffer_, PayloadKind::JsonObject);
    return absl::OkStatus();
  }

private:
  const DecoderConfig& config_;
  Buffer::OwnedImpl    body_buffer_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser
//
// Same buffer-first strategy for JSON-RPC 2.0 agent bodies. simdjson on-demand
// extracts id and method in a single pass; params is captured via raw_json()
// — a zero-copy view into the padded buffer — then parsed in a second on-demand
// pass for field extraction (tool_name, resource_uri, etc.).
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path, const DecoderConfig& config)
      : headers_(headers), http_method_(http_method), path_(path), config_(config) {}

  absl::Status feed(absl::string_view chunk) {
    if (body_buffer_.length() + chunk.size() > config_.max_body_bytes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "agent request body exceeds limit of ", config_.max_body_bytes, " bytes"));
    }
    body_buffer_.add(chunk.data(), chunk.size());
    return absl::OkStatus();
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request, PayloadStore& store) {
    if (body_buffer_.length() == 0) {
      return absl::OkStatus();
    }

    const bool capture_params = body_buffer_.length() <= config_.max_element_capture_bytes;

    const size_t body_size = body_buffer_.length();
    std::unique_ptr<char[]> buf(new char[body_size + simdjson::SIMDJSON_PADDING]());
    {
      Buffer::RawSliceVector slices = body_buffer_.getRawSlices();
      size_t offset = 0;
      for (const auto& slice : slices) {
        std::memcpy(buf.get() + offset, static_cast<const char*>(slice.mem_), slice.len_);
        offset += slice.len_;
      }
    }
    simdjson::padded_string_view padded(buf.get(), body_size,
                                        body_size + simdjson::SIMDJSON_PADDING);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (auto err = parser.iterate(padded).get(doc); err) {
      return absl::InvalidArgumentError(absl::StrCat(
          "agent body JSON parse error: ", simdjson::error_message(err)));
    }

    simdjson::ondemand::object obj;
    if (auto err = doc.get_object().get(obj); err) {
      return absl::InvalidArgumentError("agent body: expected JSON object at top level");
    }

    // Record the byte offset of the params value within buf so we can create
    // params_raw and arguments as slices of residual_params rather than copies.
    size_t params_start = 0;
    size_t params_len   = 0;

    for (auto field : obj) {
      std::string_view key;
      if (field.unescaped_key().get(key)) continue;

      if (key == "id") {
        auto val = field.value();
        simdjson::ondemand::json_type type;
        if (!val.type().get(type)) {
          if (type == simdjson::ondemand::json_type::string) {
            std::string_view sv;
            if (!val.get_string().get(sv)) request.jsonrpc_id = std::string(sv);
          } else if (type == simdjson::ondemand::json_type::number) {
            int64_t v;
            if (!val.get_int64().get(v)) request.jsonrpc_id = std::to_string(v);
          }
        }
      } else if (key == "method") {
        std::string_view v;
        if (!field.value().get_string().get(v)) request.rpc_method = std::string(v);
      } else if (key == "params" && capture_params) {
        std::string_view raw;
        if (!field.value().raw_json().get(raw)) {
          // raw points into buf; compute byte offset within the body.
          params_start = static_cast<size_t>(raw.data() - buf.get());
          params_len   = raw.size();
        }
      }
      // All other fields auto-skipped.
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

    // Store residual_params first so params_raw and arguments can be created
    // as slices into the same backing storage — zero extra allocation for
    // External refs (MmapPayloadStore), one substring copy for Inline/Buffered.
    payload.residual_params = store.store(body_buffer_, PayloadKind::JsonObject);

    if (params_len > 0) {
      payload.params_raw = store.slice(payload.residual_params, params_start, params_len);
      populateParams(buf.get() + params_start, params_len, params_start,
                     payload, store, payload.residual_params);
    }

    return absl::OkStatus();
  }

private:
  // Parses the params object and populates invocation-specific fields.
  // `params_data` points into the caller's padded body buffer at `params_body_offset`
  // bytes from the start of the body. Sub-objects (arguments, capabilities) are
  // created as slices of `residual_ref` rather than independent copies.
  static void populateParams(const char* params_data, size_t params_len,
                             size_t params_body_offset, AgentPayload& payload,
                             PayloadStore& store, const PayloadRef& residual_ref) {
    std::unique_ptr<char[]> pbuf(new char[params_len + simdjson::SIMDJSON_PADDING]());
    std::memcpy(pbuf.get(), params_data, params_len);
    simdjson::padded_string_view padded(pbuf.get(), params_len,
                                        params_len + simdjson::SIMDJSON_PADDING);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return;

    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj)) return;

    for (auto field : obj) {
      std::string_view key;
      if (field.unescaped_key().get(key)) continue;

      switch (payload.invocation) {
      case AgentInvocation::ToolsCall:
        if (key == "name") {
          std::string_view v;
          if (!field.value().get_string().get(v)) payload.tool_name = std::string(v);
        } else if (key == "arguments") {
          std::string_view raw;
          if (!field.value().raw_json().get(raw)) {
            const size_t off = static_cast<size_t>(raw.data() - pbuf.get());
            payload.arguments = store.slice(residual_ref, params_body_offset + off, raw.size());
          }
        }
        break;
      case AgentInvocation::ResourcesRead:
      case AgentInvocation::ResourcesSubscribe:
      case AgentInvocation::ResourcesUnsubscribe:
        if (key == "uri") {
          std::string_view v;
          if (!field.value().get_string().get(v)) payload.resource_uri = std::string(v);
        }
        break;
      case AgentInvocation::PromptsGet:
        if (key == "name") {
          std::string_view v;
          if (!field.value().get_string().get(v)) payload.prompt_name = std::string(v);
        } else if (key == "arguments") {
          std::string_view raw;
          if (!field.value().raw_json().get(raw)) {
            const size_t off = static_cast<size_t>(raw.data() - pbuf.get());
            payload.arguments = store.slice(residual_ref, params_body_offset + off, raw.size());
          }
        }
        break;
      case AgentInvocation::CompletionComplete:
        if (key == "ref") {
          std::string_view v;
          if (!field.value().get_string().get(v)) payload.completion_ref = std::string(v);
        }
        break;
      case AgentInvocation::Initialize:
        if (key == "capabilities") {
          std::string_view raw;
          if (!field.value().raw_json().get(raw)) {
            const size_t off = static_cast<size_t>(raw.data() - pbuf.get());
            payload.capabilities = store.slice(residual_ref, params_body_offset + off, raw.size());
          }
        }
        break;
      default:
        break;
      }
    }
  }

  const Http::RequestHeaderMap& headers_;
  const std::string             http_method_;
  const std::string             path_;
  const DecoderConfig&          config_;
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
    inference_parser_ = std::make_unique<InferenceBodyParser>(config_);
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
                                                         request_.path, config_);
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
    auto status = inference_parser_->finish(*payload, request_, payload_store_);
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
