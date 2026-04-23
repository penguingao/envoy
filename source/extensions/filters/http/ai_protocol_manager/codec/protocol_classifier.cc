#include "source/extensions/filters/http/ai_protocol_manager/codec/protocol_classifier.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

// Match `path` against a URI template like "/v1/responses/{id}/cancel".
// Template segments are delimited by '{' and '}'. On success, captured values
// are written into `params` and true is returned.
bool matchTemplate(absl::string_view path, absl::string_view tmpl,
                   absl::flat_hash_map<std::string, std::string>& params) {
  // Strip query string.
  if (auto q = path.find('?'); q != absl::string_view::npos) {
    path = path.substr(0, q);
  }

  std::vector<absl::string_view> path_segs = absl::StrSplit(path, '/');
  std::vector<absl::string_view> tmpl_segs = absl::StrSplit(tmpl, '/');

  if (path_segs.size() != tmpl_segs.size()) {
    return false;
  }

  absl::flat_hash_map<std::string, std::string> captured;
  for (size_t i = 0; i < tmpl_segs.size(); ++i) {
    absl::string_view t = tmpl_segs[i];
    if (t.size() >= 2 && t.front() == '{' && t.back() == '}') {
      captured[std::string(t.substr(1, t.size() - 2))] = std::string(path_segs[i]);
    } else if (t != path_segs[i]) {
      return false;
    }
  }
  params.merge(captured);
  return true;
}

// ── Inference classification ──────────────────────────────────────────────────

ClassifyResult classifyInference(absl::string_view method, absl::string_view path) {
  ClassifyResult r;
  r.protocol = ProtocolKind::Inference;

  // Strip query string for exact-match comparisons.
  absl::string_view clean_path = path;
  if (auto q = clean_path.find('?'); q != absl::string_view::npos) {
    clean_path = clean_path.substr(0, q);
  }

  // Bodied creates — exact path, specific verb.
  if (method == "POST" && clean_path == "/v1/chat/completions") {
    r.invocation = InferenceInvocation::ChatCompletion;
  } else if (method == "POST" && clean_path == "/v1/completions") {
    r.invocation = InferenceInvocation::Completion;
  } else if (method == "POST" && clean_path == "/v1/responses") {
    r.invocation = InferenceInvocation::ResponsesCreate;
  } else if (method == "POST" && clean_path == "/v1/embeddings") {
    r.invocation = InferenceInvocation::Embeddings;
  }
  // Resource operations — template matching extracts {id}.
  else if (method == "GET" && matchTemplate(path, "/v1/responses/{id}/input_items", r.path_params)) {
    r.invocation = InferenceInvocation::ResponsesListInputItems;
  } else if (method == "POST" && matchTemplate(path, "/v1/responses/{id}/cancel", r.path_params)) {
    r.invocation = InferenceInvocation::ResponsesCancel;
  } else if (method == "GET" && matchTemplate(path, "/v1/responses/{id}", r.path_params)) {
    r.invocation = InferenceInvocation::ResponsesRetrieve;
  } else if (method == "DELETE" && matchTemplate(path, "/v1/responses/{id}", r.path_params)) {
    r.invocation = InferenceInvocation::ResponsesDelete;
  } else {
    r.invocation = InferenceInvocation::Unknown;
  }

  return r;
}

// ── MCP classification ────────────────────────────────────────────────────────

ClassifyResult classifyMcp(absl::string_view rpc_method) {
  static const absl::flat_hash_map<absl::string_view, AgentInvocation> kMethodMap = {
      {"initialize",            AgentInvocation::Initialize},
      {"ping",                  AgentInvocation::Ping},
      {"tools/list",            AgentInvocation::ToolsList},
      {"tools/call",            AgentInvocation::ToolsCall},
      {"resources/list",        AgentInvocation::ResourcesList},
      {"resources/read",        AgentInvocation::ResourcesRead},
      {"resources/subscribe",   AgentInvocation::ResourcesSubscribe},
      {"resources/unsubscribe", AgentInvocation::ResourcesUnsubscribe},
      {"prompts/list",          AgentInvocation::PromptsList},
      {"prompts/get",           AgentInvocation::PromptsGet},
      {"sampling/createMessage",AgentInvocation::SamplingCreateMessage},
      {"completion/complete",   AgentInvocation::CompletionComplete},
      {"logging/setLevel",      AgentInvocation::LoggingSetLevel},
  };

  ClassifyResult r;
  r.protocol = ProtocolKind::AgenticMcp;
  if (auto it = kMethodMap.find(rpc_method); it != kMethodMap.end()) {
    r.invocation = it->second;
  } else {
    r.invocation = AgentInvocation::Unknown;
  }
  return r;
}

// ── A2A classification ────────────────────────────────────────────────────────

ClassifyResult classifyA2a(absl::string_view rpc_method) {
  static const absl::flat_hash_map<absl::string_view, AgentInvocation> kMethodMap = {
      {"message/send",   AgentInvocation::MessageSend},
      {"message/stream", AgentInvocation::MessageStream},
      {"tasks/submit",   AgentInvocation::TaskSubmit},
      {"tasks/get",      AgentInvocation::TaskGet},
      {"tasks/cancel",   AgentInvocation::TaskCancel},
  };

  ClassifyResult r;
  r.protocol = ProtocolKind::AgenticA2a;
  if (auto it = kMethodMap.find(rpc_method); it != kMethodMap.end()) {
    r.invocation = it->second;
  } else {
    r.invocation = AgentInvocation::Unknown;
  }
  return r;
}

// True if the path prefix suggests an OpenAI-compatible inference endpoint.
bool looksLikeInferencePath(absl::string_view path) {
  absl::string_view p = path.substr(0, path.find('?'));
  return absl::StartsWith(p, "/v1/");
}

// True if path/headers suggest an A2A endpoint rather than MCP.
bool looksLikeA2aPath(absl::string_view path) {
  absl::string_view p = path.substr(0, path.find('?'));
  return absl::StartsWith(p, "/a2a/") ||
         absl::StartsWith(p, "/api/") ||
         absl::StartsWith(p, "/.well-known/");
}

// A2A rpc_method names have a different prefix pattern ("message/*", "tasks/*")
// than MCP names ("tools/*", "resources/*", "initialize", …).
bool looksLikeA2aRpcMethod(absl::string_view rpc_method) {
  return absl::StartsWith(rpc_method, "message/") ||
         absl::StartsWith(rpc_method, "tasks/");
}

} // namespace

// ── Public entry point ────────────────────────────────────────────────────────

ClassifyResult classify(const ClassifyInput& input) {
  const absl::string_view method = input.http_method;
  const absl::string_view path   = input.path;

  // 1. When rpc_method is known, classify purely from it (ignores path for
  //    protocol selection; invocation table is the source of truth).
  if (!input.rpc_method.empty()) {
    if (looksLikeA2aRpcMethod(input.rpc_method)) {
      return classifyA2a(input.rpc_method);
    }
    return classifyMcp(input.rpc_method);
  }

  // 2. Path-based heuristics (rpc_method not yet available).

  if (looksLikeInferencePath(path)) {
    return classifyInference(method, path);
  }

  if (looksLikeA2aPath(path)) {
    // Body has not been parsed yet — invocation will be refined once rpc_method arrives.
    ClassifyResult r;
    r.protocol  = ProtocolKind::AgenticA2a;
    r.invocation = AgentInvocation::Unknown;
    return r;
  }

  // 3. Fall back to MCP for any remaining POST with JSON content-type, since
  //    MCP endpoints have no canonical path prefix.
  absl::string_view ct = input.headers.getContentTypeValue();
  if (method == "POST" && absl::StartsWith(ct, "application/json")) {
    ClassifyResult r;
    r.protocol   = ProtocolKind::AgenticMcp;
    r.invocation = AgentInvocation::Unknown;
    return r;
  }

  // NonAi — does not match any AI protocol pattern.
  return ClassifyResult{};
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
