#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/json/json_loader.h"
#include "source/common/json/json_utility.h"
#include "source/extensions/filters/http/mcp/mcp_json_parser.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser
//
// Buffers the complete OpenAI-style REST JSON body and parses it at
// onEndStream using Json::Factory::loadFromString. For large payloads the
// buffer may be flushed to PayloadStore; scalar fields (model, stream, sampling
// knobs) always stay inline. Each messages/tools element is serialised back to
// JSON via asJsonString() and then stored as a PayloadRef.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::InferenceBodyParser {
public:
  void feed(absl::string_view chunk) { body_.append(chunk.data(), chunk.size()); }

  absl::Status finish(InferencePayload& payload, AiRequest& request, PayloadStore& store,
                      const DecoderConfig& config) {
    if (body_.empty()) {
      return absl::OkStatus();
    }

    auto parse_result = Json::Factory::loadFromString(body_);
    if (!parse_result.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("inference body JSON parse error: ", parse_result.status().message()));
    }
    const Json::Object& obj = *parse_result.value();

    // "model"
    if (auto v = obj.getString("model", ""); v.ok() && !v.value().empty()) {
      payload.target.name = std::move(v.value());
    }

    // "stream"
    if (auto v = obj.getBoolean("stream", false); v.ok()) {
      request.streaming = v.value();
    }

    // Sampling scalars.
    if (auto v = obj.getDouble("temperature"); v.ok()) {
      payload.sampling.temperature = v.value();
    }
    if (auto v = obj.getDouble("top_p"); v.ok()) {
      payload.sampling.top_p = v.value();
    }
    if (auto v = obj.getInteger("max_tokens"); v.ok()) {
      payload.sampling.max_tokens = static_cast<int32_t>(v.value());
    }
    if (auto v = obj.getInteger("n"); v.ok()) {
      payload.sampling.n = static_cast<int32_t>(v.value());
    }
    if (auto v = obj.getInteger("seed"); v.ok()) {
      payload.sampling.seed = v.value();
    }
    if (auto v = obj.getStringArray("stop", /*allow_empty=*/true); v.ok()) {
      payload.sampling.stop = std::move(v.value());
    }

    // "messages" — serialize each element back to JSON, then store as PayloadRef.
    if (auto msgs = obj.getObjectArray("messages", /*allow_empty=*/true); msgs.ok()) {
      payload.messages.reserve(msgs.value().size());
      for (const auto& msg : msgs.value()) {
        payload.messages.push_back(store.store(msg->asJsonString(), PayloadKind::JsonObject));
      }
    }

    // "tools" — same pattern.
    if (auto tools = obj.getObjectArray("tools", /*allow_empty=*/true); tools.ok()) {
      payload.tools.reserve(tools.value().size());
      for (const auto& tool : tools.value()) {
        payload.tools.push_back(store.store(tool->asJsonString(), PayloadKind::JsonObject));
      }
    }

    // Store the raw body as the residual so the encoder can reconstruct all
    // fields that the mapper did not explicitly claim.
    payload.residual_params = store.store(std::move(body_), PayloadKind::JsonObject);

    (void)config;
    return absl::OkStatus();
  }

private:
  std::string body_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser
//
// Wraps McpJsonParser for streaming JSON-RPC parsing. After the full body has
// been parsed, extracts the envelope fields (jsonrpc / id / method), re-runs
// the classifier with rpc_method to determine AgentInvocation, then populates
// AgentPayload from the extracted Protobuf::Struct metadata.
// ─────────────────────────────────────────────────────────────────────────────

class RequestDecoder::AgentBodyParser {
public:
  AgentBodyParser(const Http::RequestHeaderMap& headers, absl::string_view http_method,
                  absl::string_view path)
      : headers_(headers), http_method_(http_method), path_(path),
        mcp_parser_([]() {
          auto config = Mcp::McpParserConfig::createDefault();
          config.addMethodConfig(::Envoy::Extensions::Filters::Common::Mcp::McpConstants::Methods::TOOLS_CALL,
                                 {Mcp::McpParserConfig::AttributeExtractionRule(std::string(::Envoy::Extensions::Filters::Common::Mcp::McpConstants::Paths::PARAMS_NAME)),
                                  Mcp::McpParserConfig::AttributeExtractionRule("params.arguments")});
          config.addMethodConfig(::Envoy::Extensions::Filters::Common::Mcp::McpConstants::Methods::PROMPTS_GET,
                                 {Mcp::McpParserConfig::AttributeExtractionRule(std::string(::Envoy::Extensions::Filters::Common::Mcp::McpConstants::Paths::PARAMS_NAME)),
                                  Mcp::McpParserConfig::AttributeExtractionRule("params.arguments")});
          return config;
        }()) {}

  absl::Status feed(absl::string_view chunk) {
    auto status = mcp_parser_.parse(chunk);
    if (!status.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent body JSON-RPC parse error: ", status.message()));
    }
    return absl::OkStatus();
  }

  absl::Status finish(AgentPayload& payload, AiRequest& request, PayloadStore& store) {
    // FinishParse flushes the stream parser and calls finalizeExtraction.
    auto status = mcp_parser_.finishParse();
    if (!status.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent body finalize error: ", status.message()));
    }

    const Protobuf::Struct& meta = mcp_parser_.metadata();

    // ── Envelope fields ──────────────────────────────────────────────────────

    // "id" — may be a string or a number per JSON-RPC spec.
    if (auto it = meta.fields().find("id"); it != meta.fields().end()) {
      if (it->second.has_string_value()) {
        request.jsonrpc_id = it->second.string_value();
      } else if (it->second.has_number_value()) {
        request.jsonrpc_id =
            std::to_string(static_cast<int64_t>(it->second.number_value()));
      }
      // Empty jsonrpc_id means notification (no response expected).
    }

    // "method" → rpc_method → re-classify to obtain AgentInvocation.
    request.rpc_method = mcp_parser_.getMethod();
    if (!request.rpc_method.empty()) {
      ClassifyResult r2 = classify({http_method_, path_, headers_, request.rpc_method});
      request.protocol  = r2.protocol;
      if (auto* inv = std::get_if<AgentInvocation>(&r2.invocation)) {
        payload.invocation = *inv;
      }
      payload.dialect = (r2.protocol == ProtocolKind::AgenticMcp) ? AgentDialect::Mcp
                                                                   : AgentDialect::A2a;
    }

    // ── Params fields — invocation-specific extraction ────────────────────────
    populateFromMeta(meta, payload, store);

    return absl::OkStatus();
  }

private:
  // Extracts a string value at a dot-separated path within a Protobuf::Struct.
  static std::string getStr(const Protobuf::Struct& root, absl::string_view dotted) {
    std::vector<absl::string_view> segs = absl::StrSplit(dotted, '.');
    const Protobuf::Struct* cur = &root;
    const Protobuf::Value* val = nullptr;
    for (size_t i = 0; i < segs.size(); ++i) {
      auto it = cur->fields().find(std::string(segs[i]));
      if (it == cur->fields().end()) {
        return {};
      }
      if (i == segs.size() - 1) {
        val = &it->second;
      } else {
        if (!it->second.has_struct_value()) {
          return {};
        }
        cur = &it->second.struct_value();
      }
    }
    return (val && val->has_string_value()) ? val->string_value() : std::string{};
  }

  // Serialises the Protobuf::Value at `dotted_path` to a JSON string and
  // stores it in a PayloadRef.
  static PayloadRef storeField(const Protobuf::Struct& root, absl::string_view dotted,
                               PayloadStore& store) {
    // Locate the value.
    std::vector<absl::string_view> segs = absl::StrSplit(dotted, '.');
    const Protobuf::Struct* cur = &root;
    const Protobuf::Value* val = nullptr;
    for (size_t i = 0; i < segs.size(); ++i) {
      auto it = cur->fields().find(std::string(segs[i]));
      if (it == cur->fields().end()) {
        return PayloadRef{};
      }
      if (i == segs.size() - 1) {
        val = &it->second;
      } else {
        if (!it->second.has_struct_value()) {
          return PayloadRef{};
        }
        cur = &it->second.struct_value();
      }
    }
    if (!val) {
      return PayloadRef{};
    }
    std::string json;
    Json::Utility::appendValueToString(*val, json);
    return store.store(std::move(json), PayloadKind::JsonObject);
  }

  void populateFromMeta(const Protobuf::Struct& meta, AgentPayload& payload,
                        PayloadStore& store) {
    switch (payload.invocation) {
    case AgentInvocation::ToolsCall:
      payload.tool_name = getStr(meta, "params.name");
      payload.arguments = storeField(meta, "params.arguments", store);
      break;

    case AgentInvocation::ResourcesRead:
    case AgentInvocation::ResourcesSubscribe:
    case AgentInvocation::ResourcesUnsubscribe:
      payload.resource_uri = getStr(meta, "params.uri");
      break;

    case AgentInvocation::PromptsGet:
      payload.prompt_name = getStr(meta, "params.name");
      payload.arguments   = storeField(meta, "params.arguments", store);
      break;

    case AgentInvocation::CompletionComplete:
      payload.completion_ref = getStr(meta, "params.ref");
      break;

    case AgentInvocation::Initialize:
      payload.capabilities = storeField(meta, "params.capabilities", store);
      break;

    case AgentInvocation::MessageSend:
    case AgentInvocation::TaskSubmit:
      // A2A parts live at params.message.parts[] — stored as residual for now;
      // per-part offload requires iterating the ListValue which is added in v1.
      break;

    default:
      break;
    }

    // Store the "params" value separately so the encoder can use it as the
    // base params object for invocations whose params were not fully extracted.
    if (auto it = meta.fields().find("params"); it != meta.fields().end()) {
      std::string params_json;
      Json::Utility::appendValueToString(it->second, params_json);
      payload.params_raw = store.store(std::move(params_json), PayloadKind::JsonObject);
    }

    // Store the whole parsed metadata as residual to satisfy the round-trip
    // invariant: every field the mapper didn't claim is still available.
    std::string residual;
    Json::Utility::appendStructToString(meta, residual);
    payload.residual_params = store.store(std::move(residual), PayloadKind::JsonObject);
  }

  const Http::RequestHeaderMap& headers_;
  const std::string             http_method_;
  const std::string             path_;
  Mcp::McpJsonParser            mcp_parser_;
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

  // Determine whether a body is expected. We treat any POST/PUT/PATCH as
  // potentially body-bearing; GET/DELETE/HEAD with a non-zero Content-Length
  // are unusual for AI APIs but handled conservatively.
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

  // Select body parser based on classified protocol.
  if (result.protocol == ProtocolKind::Inference) {
    InferencePayload payload;
    if (auto* inv = std::get_if<InferenceInvocation>(&result.invocation)) {
      payload.invocation = *inv;
    }
    request_.payload  = std::move(payload);
    inference_parser_ = std::make_unique<InferenceBodyParser>();
    state_            = DecodeState::ParsingInferenceBody;
  } else {
    // Agentic (MCP or A2A) — protocol refined after rpc_method arrives in body.
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
    // BodilessComplete or AwaitingHeaders — no body expected; ignore.
    return absl::OkStatus();
  }
}

absl::Status RequestDecoder::onTrailers(const Http::RequestTrailerMap& /*trailers*/) {
  // AI protocols do not use request trailers; treat trailing as end-of-stream.
  return onEndStream();
}

absl::Status RequestDecoder::onEndStream() {
  switch (state_) {
  case DecodeState::BodilessComplete:
    // Already fully populated in onHeaders.
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
  // Reset decoder so it can be reused (one decoder per filter instance is
  // typical, but resetting is cheap and avoids dangling state).
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
