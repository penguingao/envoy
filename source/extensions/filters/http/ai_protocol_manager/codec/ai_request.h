#pragma once

#include <any>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "envoy/http/header_map.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"

#include "absl/container/flat_hash_map.h"
#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

enum class ProtocolKind { NonAi, Inference, AgenticA2a, AgenticMcp };

// Per-filter scratch shared across sub-chain filters. Not serialized out.
using AiScratch = absl::flat_hash_map<std::string, std::any>;

// ─────────────────────────────────────────────────────────────────────────────
// Inference types
// ─────────────────────────────────────────────────────────────────────────────

enum class InferenceInvocation {
  Unknown,
  // Bodied creates.
  ChatCompletion,            // POST /v1/chat/completions
  Completion,                // POST /v1/completions
  ResponsesCreate,           // POST /v1/responses
  Embeddings,                // POST /v1/embeddings
  // Resource operations (body-less or small body).
  ResponsesRetrieve,         // GET    /v1/responses/{id}
  ResponsesCancel,           // POST   /v1/responses/{id}/cancel
  ResponsesDelete,           // DELETE /v1/responses/{id}
  ResponsesListInputItems,   // GET    /v1/responses/{id}/input_items
};

struct ModelTarget {
  std::string name;           // e.g. "gpt-4o", "claude-sonnet-4-6"
  std::string provider_hint;  // optional: "openai", "anthropic", "vertex"
};

struct SamplingParams {
  absl::optional<double>   temperature;
  absl::optional<double>   top_p;
  absl::optional<int32_t>  max_tokens;
  absl::optional<int32_t>  n;
  std::vector<std::string> stop;
  absl::optional<int64_t>  seed;
};

struct InferencePayload {
  InferenceInvocation invocation{InferenceInvocation::Unknown};
  ModelTarget         target;

  // Server-side resource identity (populated for Retrieve / Cancel / Delete /
  // ListInputItems). Sourced from AiRequest::path_params["id"].
  std::string resource_id;

  // Potentially large — always PayloadRef so the decoder can offload.
  std::vector<PayloadRef> messages;     // chat turns
  std::vector<PayloadRef> tools;        // tool / function definitions
  std::vector<PayloadRef> attachments;  // images, audio, files

  // Small scalar extras + anything the mapper didn't model explicitly.
  absl::flat_hash_map<std::string, std::string> extra_params;
  SamplingParams sampling;

  // Every field the mapper didn't pull apart — keeps pass-through honest.
  PayloadRef residual_params;
};

// ─────────────────────────────────────────────────────────────────────────────
// Agent types
// ─────────────────────────────────────────────────────────────────────────────

enum class AgentDialect { Unknown, A2a, Mcp };

enum class AgentInvocation {
  Unknown,
  // MCP
  Initialize,
  Ping,
  ToolsList,
  ToolsCall,
  ResourcesList,
  ResourcesRead,
  ResourcesSubscribe,
  ResourcesUnsubscribe,
  PromptsList,
  PromptsGet,
  SamplingCreateMessage,
  CompletionComplete,
  LoggingSetLevel,
  // A2A
  MessageSend,
  MessageStream,
  TaskSubmit,
  TaskGet,
  TaskCancel,
};

struct AgentTarget {
  std::string agent_id;    // logical agent / skill id for routing
  std::string session_id;  // MCP session / A2A context id (may be empty)
  std::string task_id;     // A2A task id (empty outside task ops)
};

struct AgentPayload {
  AgentDialect    dialect{AgentDialect::Unknown};
  AgentInvocation invocation{AgentInvocation::Unknown};
  AgentTarget     target;

  // Selector fields — only the ones relevant to `invocation` are populated.
  std::string tool_name;      // ToolsCall
  std::string resource_uri;   // Resources*
  std::string prompt_name;    // PromptsGet
  std::string completion_ref; // CompletionComplete ("ref/prompt" | "ref/resource")

  // Potentially large — offloadable.
  std::vector<PayloadRef> parts;  // A2A Parts | MCP content[]
  PayloadRef              arguments;    // ToolsCall.arguments, PromptsGet.arguments
  PayloadRef              capabilities; // Initialize

  // Raw JSON of the "params" field from the JSON-RPC envelope.
  // Populated for every agentic invocation; used by RequestEncoder as a
  // fallback for invocations whose params were not fully extracted into the
  // structured fields above (e.g. Initialize, ToolsList, A2A ops).
  PayloadRef params_raw;

  PayloadRef residual_params;
};

// ─────────────────────────────────────────────────────────────────────────────
// AiRequest — shared envelope + variant payload
// ─────────────────────────────────────────────────────────────────────────────

class AiRequest {
public:
  // HTTP-level identity (always populated; encoder uses these to rebuild an
  // equivalent outbound request).
  std::string http_method;
  std::string path;
  absl::flat_hash_map<std::string, std::string> path_params;
  absl::flat_hash_map<std::string, std::string> query_params;

  // Non-owning view of the downstream request headers (owned by the outer
  // filter's stream). Filters may read and mutate in place; RequestEncoder
  // reads from this when building the upstream request.
  Http::RequestHeaderMap* headers{nullptr};

  // JSON-RPC identity (populated only for JSON-RPC bodies; empty otherwise).
  std::string jsonrpc_id;   // empty ⇒ notification / non-JSON-RPC
  std::string rpc_method;   // raw "method" token when present

  ProtocolKind protocol{ProtocolKind::NonAi};
  std::variant<std::monostate, InferencePayload, AgentPayload> payload;

  // Protocol-neutral scalars (tenant, user id, request-id, routing hints).
  absl::flat_hash_map<std::string, std::string> attributes;

  bool streaming{false};

  // Not owned; outer filter owns the store.  Null only before onHeaders completes.
  PayloadStore* payload_store{nullptr};

  // Filter-to-filter scratch within this request.
  AiScratch scratch;

  // Typed accessors — return nullptr when payload holds the wrong variant.
  InferencePayload*       as_inference();
  const InferencePayload* as_inference() const;
  AgentPayload*           as_agent();
  const AgentPayload*     as_agent() const;
};

// Returns the string content of a PayloadRef. For Inline/Buffered refs this
// calls PayloadRef::toString() directly; for External refs it fetches through
// request.payload_store. Both encoder implementations use this to avoid
// PANICing on External refs produced by MmapPayloadStore.
std::string convertPayloadRefToString(const PayloadRef& ref, const AiRequest& request);

// Upgrades all External PayloadRefs in `request` to Buffered by reading them
// from the mmap store asynchronously. `on_done` is called on the dispatcher
// thread once every fetch has completed. Safe to call even if there are no
// External refs — on_done fires immediately in that case.
void prefetchExternalPayloadRefs(AiRequest& request, Event::Dispatcher& dispatcher,
                          std::function<void()> on_done);

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
