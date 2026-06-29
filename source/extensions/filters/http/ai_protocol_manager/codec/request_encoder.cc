#include "source/extensions/filters/http/ai_protocol_manager/codec/request_encoder.h"

#include "source/common/common/assert.h"
#include "source/common/json/json_streamer.h"
#include "source/common/http/utility.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "nlohmann/json.hpp"
#include "re2/re2.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// ── encodeAgentBody ───────────────────────────────────────────────────────────
//
// Produces the JSON-RPC wire body for an agentic request:
//
//   {"jsonrpc":"2.0","id":"<id>","method":"<method>","params":<params>}
//
// Three categories of invocations:
//
//  A. Fully-structured params (ToolsCall, Resources*, PromptsGet)
//     → params object rebuilt from AgentPayload fields so chain-filter
//       mutations to tool_name / resource_uri / prompt_name / arguments
//       are visible in the outgoing request.
//
//  B. All other invocations (Initialize, Ping, list ops, A2A, …)
//     → params_raw is inserted verbatim; this preserves all fields that the
//       decoder did not extract into structured form (e.g. protocolVersion,
//       clientInfo, cursor, message.parts).  Mutations to AgentPayload fields
//       not in category A are NOT reflected in the body for these invocations.
//
// The "id" field is omitted when jsonrpc_id is empty (JSON-RPC notification).

std::string RequestEncoder::encodeAgentBody(const AiRequest& request) {
  const AgentPayload* payload = request.as_agent();
  ASSERT(payload != nullptr);
  if (payload == nullptr) {
    return "{}";
  }

  std::string out;
  Json::StringOutput so(out);
  Json::StringStreamer streamer(so);

  auto root = streamer.makeRootMap();

  root->addKey("jsonrpc");
  root->addString("2.0");

  // id is omitted for notifications (empty jsonrpc_id).
  if (!request.jsonrpc_id.empty()) {
    root->addKey("id");
    root->addString(request.jsonrpc_id);
  }

  root->addKey("method");
  root->addString(request.rpc_method);

  root->addKey("params");

  switch (payload->invocation) {

  // ── Category A: fully-structured params ──────────────────────────────────

  case AgentInvocation::ToolsCall: {
    // MCP tools/call → {"name":"<tool>","arguments":<args>}
    auto params = root->addMap();
    params->addKey("name");
    params->addString(payload->tool_name);
    if (!payload->arguments.empty()) {
      params->addKey("arguments");
      params->addRawJson(convertPayloadRefToString(payload->arguments, request));
    }
    break;
  }

  case AgentInvocation::ResourcesRead:
  case AgentInvocation::ResourcesSubscribe:
  case AgentInvocation::ResourcesUnsubscribe: {
    // MCP resources/{read,subscribe,unsubscribe} → {"uri":"<uri>"}
    auto params = root->addMap();
    params->addKey("uri");
    params->addString(payload->resource_uri);
    break;
  }

  case AgentInvocation::PromptsGet: {
    // MCP prompts/get → {"name":"<prompt>","arguments":<args>}
    auto params = root->addMap();
    params->addKey("name");
    params->addString(payload->prompt_name);
    if (!payload->arguments.empty()) {
      params->addKey("arguments");
      params->addRawJson(convertPayloadRefToString(payload->arguments, request));
    }
    break;
  }

  // ── Category B: pass-through from params_raw ─────────────────────────────

  default:
    // For Initialize, Ping, list ops, CompletionComplete, LoggingSetLevel,
    // SamplingCreateMessage, and all A2A operations, use the raw params JSON
    // captured by the decoder.  Falls back to an empty object when params_raw
    // is absent (e.g. notification with no params field).
    if (payload->params_raw.empty()) {
      root->addRawJson("{}");
    } else {
      root->addRawJson(convertPayloadRefToString(payload->params_raw, request));
    }
    break;
  }

  root.reset(); // emits closing "}"
  return out;
}

namespace {

using nlohmann::json;

// ── helpers (mirrors mcp_json_rest_bridge/http_request_builder.cc) ───────────

absl::StatusOr<json> getJsonValue(const json& data, absl::string_view path) {
  std::vector<std::string> parts = absl::StrSplit(path, '.');
  json current = data;
  for (const auto& part : parts) {
    if (!current.contains(part)) {
      return absl::InvalidArgumentError(absl::StrCat("missing key: ", path));
    }
    current = current[part];
  }
  return current;
}

std::string jsonValueToString(const json& j) {
  return j.is_string() ? j.get<std::string>() : j.dump();
}

struct QueryParam {
  std::string key;
  std::string value;
};

void collectQueryParams(std::vector<QueryParam>& out, absl::string_view body_rule,
                        const json& node, const absl::flat_hash_set<std::string>& templates,
                        const std::string& path) {
  if (templates.contains(path)) {
    return;
  }
  if (!body_rule.empty()) {
    if (path == body_rule || absl::StartsWith(path, std::string(body_rule) + ".")) {
      return;
    }
  }
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      collectQueryParams(out, body_rule, it.value(), templates,
                         path.empty() ? it.key() : path + "." + it.key());
    }
  } else if (node.is_array()) {
    for (const auto& elem : node) {
      collectQueryParams(out, body_rule, elem, templates, path);
    }
  } else {
    const std::string val = jsonValueToString(node);
    out.push_back({path, Http::Utility::PercentEncoding::urlEncode(val)});
  }
}

bool removeJsonPath(json& data, absl::Span<const std::string> parts) {
  if (parts.empty() || !data.is_object()) {
    return false;
  }
  const std::string& key = parts[0];
  if (!data.contains(key)) {
    return false;
  }
  if (parts.size() == 1) {
    data.erase(key);
  } else if (removeJsonPath(data[key], parts.subspan(1)) && data[key].empty()) {
    data.erase(key);
  }
  return data.empty();
}

absl::StatusOr<RestHttpRequest> buildRestRequest(const HttpRuleProto& rule,
                                                  const json& arguments) {
  std::string method;
  std::string pattern;

  if (!rule.get().empty()) {
    method = "GET";
    pattern = rule.get();
  } else if (!rule.put().empty()) {
    method = "PUT";
    pattern = rule.put();
  } else if (!rule.post().empty()) {
    method = "POST";
    pattern = rule.post();
  } else if (!rule.delete_().empty()) {
    method = "DELETE";
    pattern = rule.delete_();
  } else if (!rule.patch().empty()) {
    method = "PATCH";
    pattern = rule.patch();
  } else {
    return absl::InvalidArgumentError("HttpRule has no HTTP method set");
  }

  // Extract template variable names from the path pattern.
  absl::flat_hash_set<std::string> templates;
  {
    absl::string_view scan = pattern;
    std::string cap;
    static const LazyRE2 kTemplateRegex = {R"(\{([a-zA-Z0-9_.]+)(?:=.*?)?\})"};
    while (RE2::FindAndConsume(&scan, *kTemplateRegex, &cap)) {
      templates.insert(cap);
    }
  }

  // Substitute template variables into the path.
  std::string url = pattern;
  for (const auto& var : templates) {
    auto val = getJsonValue(arguments, var);
    if (!val.ok()) {
      return val.status();
    }
    const std::string encoded = Http::Utility::PercentEncoding::encode(
        jsonValueToString(*val), R"( !"#$%&'()*+,:;<=>?@[\]^`{|}~)");
    const std::string var_re = "\\{" + RE2::QuoteMeta(var) + "(?:=[^}]+)?\\}";
    RE2::GlobalReplace(&url, var_re, encoded);
  }

  // Build query parameters from argument fields not consumed by path or body.
  std::vector<QueryParam> qparams;
  if (rule.body() != "*") {
    collectQueryParams(qparams, rule.body(), arguments, templates, "");
  }
  if (!qparams.empty()) {
    url += "?";
    url += absl::StrJoin(qparams, "&", [](std::string* out, const QueryParam& p) {
      absl::StrAppend(out, Http::Utility::PercentEncoding::urlEncode(p.key), "=", p.value);
    });
  }

  // Extract request body from arguments.
  std::string body_str;
  if (!rule.body().empty()) {
    json body_json;
    if (rule.body() == "*") {
      body_json = arguments;
      for (const auto& var : templates) {
        std::vector<std::string> parts = absl::StrSplit(var, '.');
        removeJsonPath(body_json, parts);
      }
    } else {
      auto field = getJsonValue(arguments, rule.body());
      if (!field.ok()) {
        return field.status();
      }
      body_json = *field;
    }
    if (!body_json.is_null()) {
      body_str = body_json.dump();
    }
  }

  return RestHttpRequest{
      .method = std::move(method),
      .path = std::move(url),
      .body = std::move(body_str),
  };
}

} // namespace

absl::optional<RestHttpRequest>
RequestEncoder::encodeAgentBodyAsRest(const AiRequest& request,
                                      const McpRestTranscoderRouteConfig& transcoder) {
  const AgentPayload* payload = request.as_agent();
  if (!payload) {
    return absl::nullopt;
  }

  const HttpRuleProto* rule = nullptr;
  json args;

  switch (payload->invocation) {
  case AgentInvocation::ToolsCall:
    rule = transcoder.toolRule(payload->tool_name);
    if (!payload->arguments.empty()) {
      args = json::parse(convertPayloadRefToString(payload->arguments, request), nullptr, /*allow_exceptions=*/false);
      if (args.is_discarded()) {
        return absl::nullopt;
      }
    } else {
      args = json::object();
    }
    break;

  case AgentInvocation::ToolsList:
    rule = transcoder.toolsListRule();
    args = json::object();
    break;

  case AgentInvocation::ResourcesList:
    rule = transcoder.resourcesListRule();
    args = json::object();
    break;

  case AgentInvocation::ResourcesRead:
    rule = transcoder.resourcesReadRule();
    // Expose resource_uri under the {uri} template variable.
    args = json{{"uri", payload->resource_uri}};
    break;

  default:
    return absl::nullopt;
  }

  if (!rule) {
    return absl::nullopt;
  }

  auto result = buildRestRequest(*rule, args);
  if (!result.ok()) {
    return absl::nullopt;
  }
  return *std::move(result);
}

// ── encodeInferenceBody ───────────────────────────────────────────────────────
//
// Re-encodes the InferencePayload back into an OpenAI-compatible JSON body.
//
// Round-trip strategy:
//   1. Parse residual_params (the full original request body) as the base JSON.
//      This preserves every field the decoder did not extract into structured
//      form (response_format, tool_choice, stream_options, user, …).
//   2. Overwrite extracted scalar fields with their current values so that
//      chain-filter mutations (e.g. model swap, temperature adjustment) appear
//      in the outgoing request.
//   3. Rebuild messages[] and tools[] from PayloadRefs.  The chain runner
//      updates PayloadRefs in-place when a Q2 onRequestItem handler calls
//      markDirty(), so this step naturally reflects per-item mutations.
//
// Bodiless invocations (Retrieve / Cancel / Delete / ListInputItems) return ""
// because those operations carry no JSON body on the wire.

std::string RequestEncoder::encodeInferenceBody(const AiRequest& request) {
  const InferencePayload* payload = request.as_inference();
  ASSERT(payload != nullptr);
  if (payload == nullptr) {
    return "";
  }

  // Bodiless resource operations have no body to encode.
  switch (payload->invocation) {
  case InferenceInvocation::ResponsesRetrieve:
  case InferenceInvocation::ResponsesCancel:
  case InferenceInvocation::ResponsesDelete:
  case InferenceInvocation::ResponsesListInputItems:
    return "";
  default:
    break;
  }

  // ── Step 1: seed from residual (full original body) ──────────────────────
  json body;
  if (!payload->residual_params.empty()) {
    body = json::parse(convertPayloadRefToString(payload->residual_params, request), nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
      body = json::object();
    }
  } else {
    body = json::object();
  }

  // ── Step 2: overlay extracted scalars (may have been mutated by chain) ───
  if (!payload->target.name.empty()) {
    body["model"] = payload->target.name;
  }
  body["stream"] = request.streaming;

  if (payload->sampling.temperature.has_value()) {
    body["temperature"] = *payload->sampling.temperature;
  }
  if (payload->sampling.top_p.has_value()) {
    body["top_p"] = *payload->sampling.top_p;
  }
  if (payload->sampling.max_tokens.has_value()) {
    body["max_tokens"] = *payload->sampling.max_tokens;
  }
  if (payload->sampling.n.has_value()) {
    body["n"] = *payload->sampling.n;
  }
  if (payload->sampling.seed.has_value()) {
    body["seed"] = *payload->sampling.seed;
  }
  if (!payload->sampling.stop.empty()) {
    body["stop"] = payload->sampling.stop;
  }

  // ── Step 3: rebuild messages[] from PayloadRefs ───────────────────────────
  // Only overwrite the "messages" key when the payload actually carries
  // messages so that we don't clobber an absent key for invocations like
  // Embeddings that don't have a messages array.
  if (!payload->messages.empty()) {
    json msgs = json::array();
    for (const auto& ref : payload->messages) {
      if (ref.empty()) {
        continue;
      }
      auto elem = json::parse(convertPayloadRefToString(ref, request), nullptr, /*allow_exceptions=*/false);
      if (!elem.is_discarded()) {
        msgs.push_back(std::move(elem));
      }
    }
    body["messages"] = std::move(msgs);
  }

  // ── Step 4: rebuild tools[] from PayloadRefs ─────────────────────────────
  if (!payload->tools.empty()) {
    json tools_arr = json::array();
    for (const auto& ref : payload->tools) {
      if (ref.empty()) {
        continue;
      }
      auto elem = json::parse(convertPayloadRefToString(ref, request), nullptr, /*allow_exceptions=*/false);
      if (!elem.is_discarded()) {
        tools_arr.push_back(std::move(elem));
      }
    }
    body["tools"] = std::move(tools_arr);
  }

  return body.dump();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
