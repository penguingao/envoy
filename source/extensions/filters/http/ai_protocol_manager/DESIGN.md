# AI Protocol Manager Filter — Design

Status: DRAFT — focused on naming and code structure. Wire-level
semantics and config proto are intentionally deferred.

## 1. Purpose

`ai_protocol_manager` is a **decoder-only** HTTP filter that:

1. Consumes the full HTTP request (method, path, headers, body,
   trailers). The body may be JSON-RPC (MCP, A2A), REST JSON (OpenAI),
   or entirely absent (e.g. `GET /v1/responses/{id}`).
2. Parses that HTTP request into a protocol-agnostic internal
   representation (`AiRequest`) that unifies the common fields of:
   - **Inference** APIs (OpenAI-style `chat.completions`, `responses`,
     including their non-body verbs like `GET`/`DELETE`/`cancel`).
   - **Agentic** protocols (A2A, MCP).

   The representation must hold enough information to reconstruct the
   original HTTP request semantically without ambiguity — see the
   **codec round-trip invariant** in §4.3.
3. Dispatches the `AiRequest` through one of two **sub filter chains**
   exposed to operators:
   - **Inference filter chain** (`inference_chain`) — for model
     invocations and response-resource operations.
   - **Agentic filter chain** (`agentic_chain`) — for agent protocol
     messages.
4. At the end of each sub-chain, a **dispatch filter** re-encodes the
   `AiRequest` back into a concrete HTTP request (same method/path
   shape, equivalent body if any) and forwards it upstream. Two
   dispatch modes are supported:
   - **Fallout mode** (terminal dispatch): the dispatch filter owns an
     `Http::AsyncClient`, bypasses the rest of the Envoy HTTP filter
     chain, and drives the full request→response cycle internally.
     This is the original mode and is described in §6.1.
   - **Chain-forward mode** (non-terminal dispatch): the dispatch
     filter re-encodes the request back into Envoy's native header/body
     structures and calls `continueDecoding()`, handing control to the
     remaining Envoy HTTP filter chain (including the `router` filter)
     for upstream delivery. The response flows back through
     `encodeHeaders` / `encodeData` / `encodeTrailers` on the same
     `AiProtocolManagerFilter` instance. This is described in §6.2.

The filter replaces what would otherwise be two parallel stacks (one per
protocol family) and lets AI-aware logic — routing, budgeting, PII
scrubbing, prompt rewriting, caching, guardrails — be written **once**
against a neutral request type.

### Non-goals (for v0)

- Multi-backend SSE fan-in (merging N upstream SSE streams into one
  downstream stream — the `mcp_router` fan-in pattern). Single-backend
  streaming and response-side filter phases **are** in scope; only the
  N→1 aggregation state machine is deferred.
- Tokenizer / cost accounting (separate filter, consumes `AiRequest`
  from filter state).
- gRPC / protobuf transports.

## 2. High-level architecture

### Request path

#### Chain-forward mode (non-terminal dispatch)

The dispatch filter re-encodes the `AiRequest` back into Envoy's native
HTTP structures and calls `continueDecoding()`. The remaining Envoy HTTP
filter chain (including the `router` filter) handles upstream delivery.
The response flows back through `AiProtocolManagerFilter`'s encoder side.

```
                            downstream HTTP request
                                     │
                                     ▼
 ┌──────────────────────────────────────────────────────────────────────┐
 │                  AiProtocolManagerFilter (Non-terminal Dispatch)     │
 │                                                                      │
 │   decodeHeaders / decodeData / decodeTrailers                        │
 │            │                                                         │
 │            ▼                                                         │
 │   ┌──────────────────┐        ┌──────────────────────────────┐       │
 │   │ RequestDecoder   │───────▶│ AiRequest (internal repr.)   │       │
 │   │ (HTTP + body,    │        │  verb/path/headers + body    │       │
 │   │  body streamed)  │        │  + PayloadRefs → PayloadStore│       │
 │   └──────────────────┘        └──────────────┬───────────────┘       │
 │                                              │                       │
 │                              classify(protocol) picks ONE chain      │
 │                               ┌──────────────┴──────────────┐        │
 │                               ▼                             ▼        │
 │                      ┌──────────────────┐       ┌──────────────────┐ │
 │                      │ InferenceChain   │       │   AgenticChain   │ │
 │                      │  (AiFilters)     │       │   (AiFilters)    │ │
 │                      └────────┬─────────┘       └────────┬─────────┘ │
 │                               ▼                          ▼           │
 │                      ┌──────────────────┐       ┌──────────────────┐ │
 │                      │ InferenceDispatch│       │  AgenticDispatch │ │
 │                      │ (non-terminal,   │       │  (non-terminal,  │ │
 │                      │ RequestEncoder)  │       │ RequestEncoder)  │ │
 │                      └────────┬─────────┘       └────────┬─────────┘ │
 │                               └───────────┬──────────────┘           │
 │                                           │                          │
 │                        decoder_callbacks_->continueDecoding()        │
 └───────────────────────────────────────────┼──────────────────────────┘
                                             │
                                             ▼
                               [other Envoy HTTP filters]
                                             │
                                             ▼
                               [Envoy router filter → upstream]
```

#### Fallout mode (terminal dispatch)

The dispatch filter owns `Http::AsyncClient` and drives the full
request→response cycle without re-entering the Envoy HTTP filter chain.

```
                            downstream HTTP request
                                     │
                                     ▼
 ┌──────────────────────────────────────────────────────────────────────┐
 │                  AiProtocolManagerFilter (Terminal Dispatch)         │
 │                                                                      │
 │   decodeHeaders / decodeData / decodeTrailers                        │
 │            │                                                         │
 │            ▼                                                         │
 │   ┌──────────────────┐        ┌──────────────────────────────┐       │
 │   │ RequestDecoder   │───────▶│ AiRequest (internal repr.)   │       │
 │   │ (HTTP + body,    │        │  verb/path/headers + body    │       │
 │   │  body streamed)  │        │  + PayloadRefs → PayloadStore│       │
 │   └──────────────────┘        └──────────────┬───────────────┘       │
 │                                              │                       │
 │                              classify(protocol) picks ONE chain      │
 │                               ┌──────────────┴──────────────┐        │
 │                               ▼                             ▼        │
 │                      ┌──────────────────┐       ┌──────────────────┐ │
 │                      │ InferenceChain   │       │   AgenticChain   │ │
 │                      │ (ordered         │       │  (ordered        │ │
 │                      │  AiFilters over  │       │   AiFilters over │ │
 │                      │  AiRequest)      │       │   AiRequest)     │ │
 │                      └────────┬─────────┘       └────────┬─────────┘ │
 │                               │ AiRequest                │ AiRequest │
 │                               ▼                          ▼           │
 │                      ┌──────────────────┐       ┌──────────────────┐ │
 │                      │ InferenceDispatch│       │  AgenticDispatch │ │
 │                      │   (terminal,     │       │    (terminal,    │ │
 │                      │ RequestEncoder)  │       │ RequestEncoder)  │ │
 │                      └────────┬─────────┘       └────────┬─────────┘ │
 │                               └──────────────┬───────────┘           │
 │                                              ▼                       │
 │                                        Http::AsyncClient             │
 └──────────────────────────────────────────────┼───────────────────────┘
                                                ▼
                                           upstream(s)
```

**Key differences from fallout mode:**

| Aspect | Fallout mode | Chain-forward mode |
|---|---|---|
| Dispatch owns `AsyncClient` | Yes | No |
| Remaining Envoy filters run | No (bypassed) | Yes |
| `router` filter drives upstream | No | Yes |
| Response arrives via | `AsyncClient` callbacks | `encodeHeaders`/`encodeData`/`encodeTrailers` |

**When to choose chain-forward mode:** operators want other Envoy filters to
see the re-encoded and/or transcoded request, or want to reuse existing cluster 
and route config with the Envoy router rather than duplicating backend selection
in the dispatch filter.

`AiRequest` is the **shared** neutral model: `RequestDecoder` emits one,
`classify()` selects which sub-chain runs, and that same `AiRequest`
(possibly mutated by chain filters) is handed to the sub-chain's
terminal `*Dispatch` filter for re-encoding. The two sub-chains differ
only in which `AiFilter` factories they draw from and which `*Dispatch`
implementation sits at their tail — not in the type that flows through
them.

### Response path

The response side flows back through the same sub-chain that handled
the request. Streaming is the common case: each SSE event / chunk is
processed individually by the chain rather than buffered.

TODO: In addition to SSE, we also need to support JSON/JSON-RPC 
response for non-streaming response.

```
                          downstream response
                                 ▲
                                 │
 ┌────────────────────────────────────────────────────────────────────┐
 │                  AiProtocolManagerFilter (same instance)           │
 │                                                                    │
 │      decoder_callbacks_->encodeHeaders / encodeData / encodeTrailers
 │                               ▲                                    │
 │                      ┌────────┴─────────┐                          │
 │                      │ ResponseEncoder  │  dirty chunks → HTTP/SSE │
 │                      │                  │  pass-through otherwise  │
 │                      └────────▲─────────┘                          │
 │                               │                                    │
 │                      ┌────────┴─────────┐                          │
 │                      │ SubChain         │  R3 onResponseEnd        │
 │                      │  (response       │  R2 onResponseChunk × N  │
 │                      │   phases over    │     (skip kinds nobody   │
 │                      │   AiResponse +   │      is interested in)   │
 │                      │   chunks)        │  R1 onResponseStart      │
 │                      └────────▲─────────┘                          │
 │                               │                                    │
 │          same sub-chain the request picked (Inference | Agentic)   │
 │                               ▲                                    │
 │                      ┌────────┴─────────┐                          │
 │                      │ ResponseDecoder  │  upstream headers →      │
 │                      │  (HTTP/SSE →     │  AiResponse (summary),   │
 │                      │   AiResponse +   │  then per-event →        │
 │                      │   AiResponseChunk│  AiResponseChunk stream  │
 │                      │   stream)        │                          │
 │                      └────────▲─────────┘                          │
 │                               │                                    │
 │                      ┌────────┴─────────┐                          │
 │                      │ *Dispatch filter │  (owns AsyncStream)      │
 │                      └────────▲─────────┘                          │
 │                               │                                    │
 └────────────────────────────────────────────────────────────────────┘
                                 ▲
                                 │
                        upstream response(s)
```

The response diagram is drawn flow-up so that **downstream sits at the
top and upstream at the bottom in both diagrams** — you can stack the
two and the upstream edges line up, giving one continuous
downstream→upstream→downstream picture. R1 is at the bottom (earliest,
closest to the upstream input) and R3 at the top (latest, closest to
the downstream output), mirroring how request-side Q1 sits near the
top and dispatch sits at the bottom.

Symmetry with the request side: `AiResponse` plays the envelope role
(`summary` variant + `headers` + `http_status`), `AiResponseChunk` plays
the per-item role with the same dirty-flag / skip-optimization
mechanics. The sub-chain is the same instance as on the request path,
so per-request `scratch` state set by `onRequestMetadata` is still
visible to `onResponseStart`.

### Transcoding: JSON-RPC <-> JSON REST

Transcoding between JSON-RPC and JSON REST onboards wildly-deployed REST
API services to rapidly emerging Agents and MCP Clients. The transcoder
naturally sits at the `RequestEncoder` where the high-level AI
representation is lowered to the HTTP representation prior to dispatch.

#### Request Decode phase

`RequestDecoder` parses the JSON-RPC body and extracts its structured
fields (`method`, `params`, `id`) into the protocol-agnostic `AiRequest`
/ `AgentPayload`. The JSON-RPC envelope is discarded at this point — the
filter works entirely in terms of `AgentInvocation`, `tool_name`,
`arguments`, etc.

#### Request Encode phase

Transcoding happens when re-encoding the `AiRequest` back to HTTP.
`RequestEncoder` serializes the structured fields as a plain JSON REST
body (e.g. `{ "name": "...", "arguments": {...} }`) rather than wrapping
them back in a JSON-RPC object. The HTTP method, path, and
`Content-Type: application/json` header are rewritten to match the target
REST API's conventions.

#### Wire format selection

The dispatch filter selects the output wire format based on the backend's
configured protocol. If the backend is REST-native the encoder emits REST
JSON; if the backend expects JSON-RPC (e.g. a native MCP server) the
encoder re-wraps the fields into a JSON-RPC envelope, preserving the
original `jsonrpc_id` and `rpc_method`. The `AiRequest` representation is
identical in both cases — only the serialization path inside
`RequestEncoder` changes.

#### Path and host rewriting

Because the decoded `AiRequest` carries the logical invocation
(`AgentInvocation::ToolsCall`, `tool_name = "search"`) rather than raw
RPC fields, the encoder could construct the appropriate REST path (e.g.
`POST /tools/search`) instead of the original JSON-RPC endpoint (e.g.
`POST /mcp`). Placing the transcoder in front of the router enables
route/cluster re-selection in Envoy.

### Case Study: Transcoding

Implements MCP transcoding with AI Filter Chain: 
- No double parsing: Parsing only happens once at RequestDecoder, transcoding is performed on native AI message after it traverses the AI filter chain.

- Seamless "Lowering": Transcoding is now a natural lifecycle step where high-level AI intents are lowered to HTTP protocols—be it JSON-RPC for MCP-native backends or JSON REST for REST services.


The whole transcoding flow :

```
   MCP client                AiProtocolManagerFilter                   REST backend
      │                              │                                       │
      │  POST /mcp                   │                                       │
      │  {"jsonrpc":"2.0",           │                                       │
      │   "method":"tools/call",     │                                       │
      │   "params":{"name":"search", │                                       │
      │    "arguments":{"query":"hello"}}}                                   │
      │─────────────────────────────▶│                                       │
      │                              │ RequestDecoder                        │
      │                              │   strips JSON-RPC envelope            │
      │                              │   ┌─────────────────────────────┐     │
      │                              │   │ AgentPayload                │     │
      │                              │   │  invocation = ToolsCall     │     │
      │                              │   │  tool_name  = "search"      │     │
      │                              │   │  arguments  = {query:hello} │     │
      │                              │   └─────────────────────────────┘     │
      │                              │                                       │
      │                              │ AgenticChain                          │
      │                              │   ┌─────────────────────────────┐     │
      │                              │   │ McpAuthFilter               │     │
      │                              │   │  auth/authz on AgentPayload │     │
      │                              │   │  not on raw HTTP            │     │
      │                              │   └─────────────────────────────┘     │
      │                              │                                       │
      │                              │ AgenticDispatch                       │
      │                              │   ┌─────────────────────────────────┐ │
      │                              │   │ RequestEncoder                  │ │
      │                              │   │                                 │ │
      │                              │   │ transcoder config present?      │ │
      │                              │   │                                 │ │
      │                              │   │ YES → encodeAgentBodyAsRest()   │ │
      │                              │   │   method = GET                  │ │
      │                              │   │   path   = /api/search          │ │
      │                              │   │            ?query=hello         │ │
      │                              │   │   body   = (empty)              │ │
      │                              │   │                                 │ │
      │                              │   │ NO  → encodeAgentBody()         │ │
      │                              │   │   re-wraps as JSON-RPC          │ │
      │                              │   └─────────────────────────────────┘ │
      │                              │   mutate headers + addDecodedData()   │
      │                              │   continueDecoding() → router filter  │
      │                              │──────────────────────────────────────▶│
      │                              │                    GET /api/search    │
      │                              │                        ?query=hello   │

```

## 3. Directory & file layout

All files live under
`source/extensions/filters/http/ai_protocol_manager/`. Public protos live
under `api/envoy/extensions/filters/http/ai_protocol_manager/v3/`.

```
ai_protocol_manager/
├── BUILD
├── DESIGN.md
│
│   # Envoy filter plumbing
├── config.h / config.cc                 # NamedHttpFilterConfigFactory
├── filter.h / filter.cc                 # AiProtocolManagerFilter
├── filter_config.h / filter_config.cc   # AiProtocolManagerConfig, stats
│
│   # Protocol-neutral request model + HTTP ↔ AiRequest codec
├── codec/
│   ├── ai_request.h / ai_request.cc         # AiRequest envelope + enums
│   ├── inference_payload.h / .cc            # InferencePayload variant
│   ├── agent_payload.h / .cc                # AgentPayload variant
│   ├── ai_item.h / ai_item.cc               # AiItem + Message/Tool/Attachment
│   ├── ai_response.h / ai_response.cc       # AiResponse envelope + summaries
│   ├── ai_response_chunk.h / .cc            # AiResponseChunk + chunk variants
│   ├── ai_payload.h / ai_payload.cc         # PayloadRef, PayloadStore iface
│   ├── request_decoder.h / .cc              # HTTP request → AiRequest
│   ├── request_encoder.h / .cc              # AiRequest → HTTP request
│   ├── response_decoder.h / .cc             # upstream HTTP/SSE → AiResponse + chunks
│   ├── response_encoder.h / .cc             # AiResponse + chunks → downstream HTTP/SSE
│   ├── json_rpc_parser.h / .cc              # streaming JSON-RPC body parser
│   ├── inference_mapping.h / .cc            # OpenAI ↔ AiRequest/Response
│   ├── agent_mapping.h / .cc                # A2A + MCP ↔ AiRequest/Response
│   └── protocol_classifier.h / .cc          # verb+path+headers → ProtocolKind
│
│   # Sub-chain machinery (the “ergonomic AI filter chain” surface)
├── chain/
│   ├── ai_filter.h                          # AiFilter interface (pure virtual)
│   ├── ai_filter_callbacks.h                # AiFilterCallbacks (chain API)
│   ├── ai_filter_chain.h / .cc              # ordered runner, shared by both kinds
│   ├── ai_filter_factory.h                  # registration for sub-chain filters
│   ├── inference_chain.h / .cc              # InferenceChain (typed façade)
│   └── agentic_chain.h / .cc                  # AgenticChain (typed façade)
│
│   # Terminal dispatch filters at the tail of each sub-chain
└── dispatch/
    ├── ai_dispatch_filter.h / .cc           # shared base (encode + async client)
    ├── inference_dispatch.h / .cc           # InferenceDispatchFilter
    └── agentic_dispatch.h / .cc             # AgenticDispatchFilter
```

### Namespace

```cpp
Envoy::Extensions::HttpFilters::AiProtocolManager
  ::Codec       // codec/
  ::Chain       // chain/
  ::Dispatch    // dispatch/
```

## 4. Core types

### 4.1 `AiRequest` — shared envelope + variant payload

`codec/ai_request.h`. The request is a shared envelope holding fields
that are genuinely protocol-neutral, plus a `std::variant` payload
carrying the protocol-specific body. Cross-cutting sub-chain filters
(PII scrub, rate limit, budget, logging) take `AiRequest&` and never
see the variant; specialized filters and the terminal `*Dispatch`
filters pull out the variant they expect.

#### Envelope

```cpp
// codec/ai_request.h

enum class ProtocolKind { Unknown, Inference, AgenticA2a, AgenticMcp };

// Per-filter scratch shared across sub-chain filters (not cross-request,
// not serialized back out).
using AiScratch = absl::flat_hash_map<std::string, std::any>;

class AiRequest {
public:
  // --- HTTP-level identity (always populated; encoder uses these to
  //     rebuild an equivalent outbound request). ---
  std::string http_method;   // "GET", "POST", "DELETE", "PATCH", …
  std::string path;           // e.g. "/v1/responses/resp_abc123"
  // Parsed path parameters (e.g. {"response_id": "resp_abc123"})
  // populated by the classifier/mapper from a path pattern.
  absl::flat_hash_map<std::string, std::string> path_params;
  // Raw query string key/values.
  absl::flat_hash_map<std::string, std::string> query_params;
  // Non-owning view of the downstream request headers (owned by the
  // outer filter's stream as passed into decodeHeaders). Filters may
  // read and mutate in place; RequestEncoder reads from this when
  // building the upstream request. Uses Envoy's native map so we keep
  // case-insensitivity, inline header slots, and multi-value support.
  Http::RequestHeaderMap* headers{nullptr};

  // --- JSON-RPC identity (populated only for JSON-RPC bodies;
  //     empty for REST-ish or bodiless requests). ---
  std::string jsonrpc_id;    // empty ⇒ notification / non-JSON-RPC
  std::string rpc_method;    // raw "method" token when present

  // --- Protocol discriminator + variant payload. ---
  ProtocolKind protocol{ProtocolKind::NonAi};
  std::variant<std::monostate, InferencePayload, AgentPayload> payload;

  // --- Protocol-neutral small scalars (tenant, user id, request-id,
  //     routing hints). Cross-cutting filters read from here.
  absl::flat_hash_map<std::string, std::string> attributes;

  // --- Streaming intent (OpenAI stream:true, A2A/MCP SSE subscribe,
  //     Responses GET with stream=true reattach). ---
  bool streaming{false};

  // --- Payload offload: not owned; outer filter owns the store. ---
  PayloadStore* payload_store{nullptr};

  // --- Filter-to-filter scratch within this request. ---
  AiScratch scratch;

  // --- Typed accessors. Return nullptr on wrong variant. ---
  InferencePayload*       as_inference();
  const InferencePayload* as_inference() const;
  AgentPayload*           as_agent();
  const AgentPayload*     as_agent() const;
};
```

#### Inference variant — `codec/inference_payload.h`

```cpp
enum class InferenceInvocation {
  Unknown,
  // Bodied creates.
  ChatCompletion,            // POST /v1/chat/completions
  Completion,                // POST /v1/completions
  ResponsesCreate,           // POST /v1/responses
  Embeddings,                // POST /v1/embeddings
  // Resource ops on prior responses (body-less or small body).
  ResponsesRetrieve,         // GET    /v1/responses/{id}
  ResponsesCancel,           // POST   /v1/responses/{id}/cancel
  ResponsesDelete,           // DELETE /v1/responses/{id}
  ResponsesListInputItems,   // GET    /v1/responses/{id}/input_items
  // (Audio, Moderations, Images — added as needed.)
};

struct ModelTarget {
  std::string name;            // "gpt-4o-mini", "claude-sonnet-4-6", …
  std::string provider_hint;   // optional: "openai", "anthropic", "vertex"
};

struct SamplingParams {
  absl::optional<double>   temperature;
  absl::optional<double>   top_p;
  absl::optional<int32_t>  max_tokens;
  absl::optional<int32_t>  n;
  std::vector<std::string> stop;
  absl::optional<int64_t>  seed;
  // Rarer knobs (presence_penalty, frequency_penalty, logprobs, …)
  // live in InferencePayload::extra_params rather than bloating this.
};

struct InferencePayload {
  InferenceInvocation invocation{InferenceInvocation::Unknown};
  ModelTarget         target;

  // Server-side resource identity (populated for ResponsesRetrieve /
  // Cancel / Delete / ListInputItems; empty for bodied creates).
  // Sourced from AiRequest::path_params and used by dispatch to route
  // back to the backend that originally produced the resource.
  std::string resource_id;

  // Potentially large — always PayloadRef so the decoder can offload.
  std::vector<PayloadRef> messages;      // chat turns
  std::vector<PayloadRef> tools;         // tool / function definitions
  std::vector<PayloadRef> attachments;   // images, audio, files

  // tool_choice, response_format, service_tier, user, plus any params
  // the mapper didn't claim.
  absl::flat_hash_map<std::string, std::string> extra_params;

  SamplingParams sampling;

  // Everything the mapper didn't pull apart — keeps pass-through honest.
  PayloadRef residual_params;
};
```

#### Agent variant — `codec/agent_payload.h`

```cpp
enum class AgentDialect { Unknown, A2a, Mcp };

enum class AgentInvocation {
  Unknown,
  // MCP
  Initialize, Ping,
  ToolsList, ToolsCall,
  ResourcesList, ResourcesRead, ResourcesSubscribe, ResourcesUnsubscribe,
  PromptsList, PromptsGet,
  SamplingCreateMessage, CompletionComplete, LoggingSetLevel,
  // A2A
  MessageSend, MessageStream,
  TaskSubmit, TaskGet, TaskCancel,
  // Notifications folded in here (NotificationInitialized, …) when we
  // need to route them.
};

struct AgentTarget {
  std::string agent_id;     // logical agent / skill id for routing
  std::string session_id;   // MCP session / A2A context id (may be empty)
  std::string task_id;      // A2A task id (empty outside task ops)
};

struct AgentPayload {
  AgentDialect     dialect{AgentDialect::Unknown};
  AgentInvocation  invocation{AgentInvocation::Unknown};
  AgentTarget      target;

  // Selector fields — small, protocol-specific, filled based on
  // invocation. Only the ones relevant to `invocation` are populated.
  std::string tool_name;       // ToolsCall
  std::string resource_uri;    // Resources*
  std::string prompt_name;     // PromptsGet
  std::string completion_ref;  // CompletionComplete ("ref/prompt" | "ref/resource")

  // Potentially large — offloadable.
  std::vector<PayloadRef> parts;        // A2A Parts | MCP content[]
  PayloadRef              arguments;    // ToolsCall.arguments, PromptsGet.arguments
  PayloadRef              capabilities; // Initialize

  PayloadRef residual_params;
};
```

#### Design notes

1. **Variant inside `AiRequest`, not base class**: avoids heap
   allocation per request, keeps cross-cutting filters taking
   `AiRequest&` without virtual dispatch, and lets `std::visit` work
   for exhaustive handling in dispatch filters.
2. **`ModelTarget` vs `AgentTarget` don't unify**: an inference target
   names a *model*; an agent target names an *agent / session / task*.
   Hoisting a shared "target" into the envelope would paper over that.
3. **One invocation enum per variant**: keeps the inference mapper
   from ever considering MCP values and vice versa, and lets each
   sub-chain's `AiFilter` factories validate config against only its
   variant.
4. **Three field tiers, on purpose**:
   - `attributes` — protocol-neutral scalars that arrived with the
     request and cross-cutting filters care about (tenant, user id).
   - `InferencePayload::extra_params` / variant residuals —
     protocol-specific JSON fields the mapper didn't model.
   - `scratch` — runtime-only, filter-to-filter data, never
     serialized back out.
5. **`AiResponse`**: unified for v0 (status + headers + body). Apply
   the same envelope+variant pattern if response-side logic grows
   protocol-specific (OpenAI chunk framing vs A2A event types).
6. **Open**: should `AgentPayload` split into `A2aPayload` /
   `McpPayload`? Kept unified because fields overlap heavily and
   `dialect` is already a discriminator; revisit if MCP/A2A diverge
   more than expected.

### 4.2 `PayloadRef` + `PayloadStore` — offload boundary

`codec/ai_payload.h`. Keeps large blobs out of filter memory.

```cpp
class PayloadRef {
public:
  enum class Storage { Inline, Buffered, External };
  Storage storage() const;
  absl::string_view inline_view() const;           // Inline
  const Buffer::Instance& buffered() const;        // Buffered
  absl::string_view external_handle() const;       // External (opaque URI)
  size_t size() const;
};

class PayloadStore {
public:
  virtual ~PayloadStore() = default;
  // Stash raw bytes, return a ref the encoder can later resolve.
  virtual PayloadRef store(Buffer::Instance&& data, PayloadKind kind) = 0;
  // Materialize a ref back into a buffer (may be async for External).
  virtual void fetch(const PayloadRef&, FetchCallback cb) = 0;
};
```

Initial implementations:
- `InMemoryPayloadStore` (default, threshold-bounded).
- `FileApiPayloadStore` (offloads above threshold to the configured
  file API / object store).

The decoder owns a `PayloadStore*` and, when a field crosses a
configured byte threshold during streaming, emits an `External` ref
instead of an `Inline`/`Buffered` one. The encoder resolves refs back
into the outbound JSON-RPC buffer.

### 4.3 `RequestDecoder` / `RequestEncoder` — HTTP ↔ `AiRequest`

The codec's job is to translate between a wire-level HTTP request and
`AiRequest`, regardless of whether there is a body. JSON-RPC / REST
body parsing is one sub-step driven by the protocol mapper; a bodiless
request (e.g. `GET /v1/responses/{id}`) is a valid, complete input.

```cpp
// codec/request_decoder.h

class RequestDecoder : public Logger::Loggable<Logger::Id::filter> {
public:
  RequestDecoder(const DecoderConfig&, PayloadStore&);

  // Called as soon as headers arrive. Populates verb/path/headers on
  // AiRequest, runs the classifier, picks the mapper, and decides
  // whether a body is expected.
  absl::Status onHeaders(const Http::RequestHeaderMap&);

  // Body is streamed incrementally; no-op when no body is expected.
  absl::Status onData(absl::string_view chunk);

  absl::Status onTrailers(const Http::RequestTrailerMap&);
  absl::Status onEndStream();

  absl::StatusOr<AiRequest> take();   // owns result
};

// codec/request_encoder.h

class RequestEncoder {
public:
  RequestEncoder(const EncoderConfig&, PayloadStore&);

  // Emits the outbound verb + path + headers + (optional) body buffer.
  // Body production may require async PayloadStore::fetch calls for
  // External refs — the caller drives the state machine.
  struct EncodedRequest {
    std::string                http_method;
    std::string                path;
    Http::RequestHeaderMapPtr  headers;
    Buffer::InstancePtr        body;       // may be null for bodiless verbs
  };
  absl::StatusOr<EncodedRequest> encode(const AiRequest&);
};
```

#### RequestDecoder — internal design

##### State machine

```cpp
enum class DecodeState {
  AwaitingHeaders,
  BodilessComplete,       // no body expected; AiRequest is ready after onHeaders
  ParsingInferenceBody,   // streaming JSON → InferenceBodyParser
  ParsingAgentBody,       // streaming JSON-RPC → AgentBodyParser
  BodyComplete,           // onEndStream received; take() is valid
  Error,
};

class RequestDecoder {
  DecodeState state_{DecodeState::AwaitingHeaders};
  AiRequest   request_;
  PayloadStore& payload_store_;

  // Exactly one is non-null after onHeaders picks a body parser.
  std::unique_ptr<InferenceBodyParser> inference_parser_;
  std::unique_ptr<AgentBodyParser>     agent_parser_;
};
```

##### `onHeaders`

```
1. Copy http_method and path into request_.

2. Parse path into segments; extract raw query string.
   Populate request_.query_params from the query string.

3. Run ProtocolClassifier::classify({http_method, path, headers, ""})
   → ClassifyResult { protocol, maybe invocation, path_params }
   Copy path_params into request_.path_params.

4. Set request_.protocol.

5. Determine whether a body is expected:

   Bodiless verbs (GET, DELETE) or body-free POSTs (e.g. cancel):
     → Call the appropriate mapper with path_params + query_params
       to populate the payload variant (e.g. ResponsesRetrieve,
       ResponsesDelete, AgentInvocation::TaskCancel).
     → state_ = BodilessComplete
     → return OK

   POST / PATCH with body expected:
     → Inspect Content-Type:
         "application/json"          → pick InferenceBodyParser (if
                                         protocol == Inference)
         "application/json" (RPC)    → pick AgentBodyParser (if
                                         protocol == Agentic*)
         (classifier drives the choice; Content-Type is a hint)
     → Construct the chosen parser.
     → state_ = ParsingInferenceBody | ParsingAgentBody
```

##### `onData` — streaming body

Each `decodeData` call delivers a chunk of raw body bytes (any size;
TCP may fragment arbitrarily). Both body parsers implement a
streaming SAX-style JSON interface: they consume bytes incrementally
and emit structured field events without buffering the whole JSON
document.

```
switch (state_) {
  case ParsingInferenceBody: inference_parser_->feed(chunk); break;
  case ParsingAgentBody:     agent_parser_->feed(chunk);     break;
  default:                   /* no body expected, ignore */   break;
}
```

Large-field offload happens inside the parser (see below): as soon as
a string field crosses the configured byte threshold the parser
calls `payload_store_.store(...)` and keeps only the returned
`PayloadRef`, never holding the full bytes in filter memory.

##### `onEndStream`

```
switch (state_) {
  case BodilessComplete:     break;  // already populated by onHeaders
  case ParsingInferenceBody:
    inference_parser_->finish();     // validate JSON is complete
    // InferencePayload is now fully populated in request_.payload
    break;
  case ParsingAgentBody:
    agent_parser_->finish();
    // AgentPayload is now fully populated in request_.payload
    break;
  default: return error;
}
state_ = BodyComplete;
request_.payload_store = &payload_store_;
```

Trailers (`onTrailers`) are forwarded to the active parser for any
trailer-encoded metadata; in practice AI APIs do not use trailers on
the request side, so this is a no-op.

---

##### `InferenceBodyParser` — JSON body → `InferencePayload`

Used when `request_.protocol == ProtocolKind::Inference`.
The body is an OpenAI-style REST JSON object:

```json
{
  "model":        "gpt-4o",
  "messages":     [...],
  "tools":        [...],
  "stream":       true,
  "temperature":  0.7,
  "max_tokens":   1024,
  ...
}
```

The parser walks the top-level object using a streaming JSON event
loop. Field dispatch table:

| JSON key           | Maps to                                    | Offloadable |
|--------------------|--------------------------------------------|-------------|
| `"model"`          | `InferencePayload::target.name`            | no          |
| `"messages"`       | `InferencePayload::messages` (PayloadRef[])| **yes**     |
| `"tools"`          | `InferencePayload::tools` (PayloadRef[])   | **yes**     |
| `"stream"`         | `AiRequest::streaming`                     | no          |
| `"temperature"`    | `SamplingParams::temperature`              | no          |
| `"top_p"`          | `SamplingParams::top_p`                    | no          |
| `"max_tokens"`     | `SamplingParams::max_tokens`               | no          |
| `"n"`              | `SamplingParams::n`                        | no          |
| `"seed"`           | `SamplingParams::seed`                     | no          |
| `"stop"`           | `SamplingParams::stop`                     | no          |
| `"stream_options"` | `InferencePayload::extra_params`           | no          |
| everything else    | `InferencePayload::residual_params`        | **yes**     |

**Array-element offload** (`"messages"` and `"tools"`):

Each array element is streamed into a local `Buffer::OwnedImpl`.
When the element's closing `}` arrives, the accumulated buffer is
evaluated:

```
if (element_buffer.length() > config_.max_inline_bytes) {
  PayloadRef ref = payload_store_.store(std::move(element_buffer),
                                        PayloadKind::JsonObject);
  // ref.storage() == External
} else {
  PayloadRef ref = PayloadRef::inline(element_buffer.toString());
  // ref.storage() == Inline
}
messages.push_back(ref);
```

This means filters downstream never see more than
`max_inline_bytes` of a single message in filter memory; the rest is
referenced by handle and fetched only when a filter calls
`AiFilterCallbacks` methods that trigger `PayloadStore::fetch`.

**Multimodal content** (`"messages[i].content"` as array):

When a message's `content` field is itself an array of parts (text /
image_url / audio / file), each part is offloaded independently
using the same threshold logic. The in-memory `PayloadRef` list
replaces the content array on the `Message` struct; the
`RequestEncoder` reassembles the JSON array from refs at re-encode
time.

**`"stream"` detection**:

`AiRequest::streaming` is set to `true` as soon as the boolean value
is parsed (it is a top-level scalar so it arrives early in the
stream, before `"messages"`).

---

##### `AgentBodyParser` — JSON-RPC body → `AgentPayload`

Used when `request_.protocol == ProtocolKind::AgenticMcp` or
`AgenticA2a`. The body is a JSON-RPC 2.0 object:

```json
{
  "jsonrpc": "2.0",
  "id":      "42",
  "method":  "tools/call",
  "params":  { ... }
}
```

The parser has two stages:

**Stage 1 — envelope parsing** (runs until `"params"` key is seen):

| JSON key    | Maps to                 | Action                          |
|-------------|-------------------------|---------------------------------|
| `"jsonrpc"` | version check           | warn if not `"2.0"`             |
| `"id"`      | `AiRequest::jsonrpc_id` | copy value (string or number)   |
| `"method"`  | `AiRequest::rpc_method` | → **re-run classifier** (below) |
| `"params"`  | *transitions to stage 2*| select `AgentParamsParser`      |

**Re-running the classifier on `"method"`**:

Because MCP and A2A use the same `Content-Type: application/json`,
the HTTP headers alone may not uniquely identify the invocation.
When the `"method"` token arrives:

```
ClassifyResult r2 = classify({
    request_.http_method, request_.path, *request_.headers,
    request_.rpc_method   // now non-empty
});
// Merge r2 into request_: refine protocol, set invocation
AgentPayload& ap = std::get<AgentPayload>(request_.payload);
ap.invocation = std::get<AgentInvocation>(r2.invocation);
ap.dialect    = (r2.protocol == ProtocolKind::AgenticMcp)
                    ? AgentDialect::Mcp : AgentDialect::A2a;
// Select the correct AgentParamsParser for this invocation.
params_parser_ = makeParamsParser(ap.invocation);
```

**Stage 2 — `"params"` parsing** (dispatched per invocation):

Each `AgentParamsParser` knows the schema for its invocation.
Examples:

```
MCP ToolsCall  (AgentInvocation::ToolsCall):
  "name"        → AgentPayload::tool_name  (scalar, no offload)
  "arguments"   → AgentPayload::arguments  (PayloadRef, offloadable)
  else          → AgentPayload::residual_params

MCP Initialize  (AgentInvocation::Initialize):
  "protocolVersion" → extra attributes
  "capabilities"    → AgentPayload::capabilities (PayloadRef whole object)
  "clientInfo"      → AgentPayload::residual_params

MCP ResourcesRead  (AgentInvocation::ResourcesRead):
  "uri"         → AgentPayload::resource_uri
  else          → AgentPayload::residual_params

A2A MessageSend  (AgentInvocation::MessageSend):
  "message.role"     → AgentTarget / extra
  "message.parts[i]" → AgentPayload::parts[i]  (offloadable per-part)
  "message.messageId"→ attributes
  else               → AgentPayload::residual_params

A2A TaskSubmit  (AgentInvocation::TaskSubmit):
  "id"               → AgentPayload::target.task_id
  "message.parts[i]" → AgentPayload::parts[i]  (offloadable)
  else               → AgentPayload::residual_params
```

**JSON-RPC notification** (no `"id"` field):

`jsonrpc_id` remains empty; `AiRequest::streaming` is set `false`.
Notifications are dispatched through the chain identically to
method calls; the dispatch filter checks `jsonrpc_id.empty()` to
decide whether to expect a response.

**Implementation notes on parser choice:**

- The JSON-RPC streaming parser is adapted from
  `source/extensions/filters/http/mcp/mcp_json_parser.h`
  (`codec/json_rpc_parser.h`). It drives `AgentBodyParser`.
- `InferenceBodyParser` uses a separate REST-JSON streaming helper
  (does not share the JSON-RPC envelope logic).
- Both parsers expose the same `feed(absl::string_view) / finish()`
  interface to `RequestDecoder`, making `onData` dispatch uniform.
- For a bodiless request the body helpers are skipped entirely; the
  mapper populates the variant from `path_params` / `query_params` /
  `headers`.

#### Codec round-trip invariant

For any HTTP request R accepted by `RequestDecoder`:

> `RequestEncoder.encode(RequestDecoder(R))` must produce a request R'
> that a compliant backend interprets **semantically identically** to R.

"Semantically identical" (not byte-identical) means:

- Same HTTP method and the same path (modulo a backend-specific
  rewrite the dispatch filter may deliberately apply).
- Same set of semantically meaningful request headers. Hop-by-hop
  headers and transport-level headers (`Content-Length`, `Host`,
  `Authorization` for the upstream) may be regenerated.
- Same JSON body shape when a body is present: every field the mapper
  modeled appears with the same value, every field it didn't model is
  carried through `residual_params` verbatim. JSON key order and
  whitespace are **not** preserved.
- Bodiless requests round-trip with an empty body.

Any field a mapper cannot losslessly represent must either extend the
model or be stashed in the variant's residual — dropping is not
acceptable. This invariant is what makes the filter safe to insert
transparently in front of AI backends.

### 4.4 Protocol classification

`codec/protocol_classifier.h`:

```cpp
struct ClassifyInput {
  absl::string_view http_method;
  absl::string_view path;
  const Http::RequestHeaderMap& headers;
  // JSON-RPC "method" token (empty for REST / bodiless).
  absl::string_view rpc_method;
};

struct ClassifyResult {
  ProtocolKind protocol;
  // Populated when known from headers/path alone (before body parsing).
  absl::variant<absl::monostate, InferenceInvocation, AgentInvocation>
      invocation;
  // Extracted path params (e.g. response_id), ready to copy into
  // AiRequest::path_params.
  absl::flat_hash_map<std::string, std::string> path_params;
};

ClassifyResult classify(const ClassifyInput&);
```

Classification combines HTTP verb, path pattern, `content-type`, an
explicit config override, and — when available — the JSON-RPC `method`
token. Because it runs at `decodeHeaders`, it gets the first shot at
routing before any body has arrived, which is what lets bodiless
requests like `GET /v1/responses/{id}` classify correctly without ever
calling into a body parser.

### 4.5 `AiItem` — materialized view of a large payload

`codec/ai_item.h`. `PayloadRef` is the storage-side handle; `AiItem`
is the runtime-side materialized view that filter authors see during
per-item callbacks. It exists only for the duration of one
`onRequestItem` invocation — the runtime fetches the bytes from
`PayloadStore`, hands the filter a concrete value, and re-stores on
return if the filter mutated it.

```cpp
enum class AiItemKind { Message, Tool, Attachment };

struct Message {            // chat turn / A2A part / MCP content
  std::string role;         // "user", "assistant", "system", "tool"
  std::string text;         // primary text content (materialized)
  std::vector<ContentPart> parts;   // multimodal parts (text/image/audio/…)
  absl::flat_hash_map<std::string, std::string> attributes;
};

struct Tool {               // tool / function definition
  std::string name;
  std::string description;
  std::string schema_json;  // JSON-schema for arguments
  absl::flat_hash_map<std::string, std::string> attributes;
};

struct Attachment {         // image, audio, file, blob
  std::string mime_type;
  std::string filename;     // optional
  std::string bytes;        // materialized; may be very large
  absl::flat_hash_map<std::string, std::string> attributes;
};

class AiItem {
public:
  AiItemKind kind() const;
  size_t     index() const;         // position within its kind list

  // Mutation tracking — filter must call markDirty() (or mutate via
  // the helper setters, TBD) if it changed anything. Clean items
  // skip the re-store step back into PayloadStore.
  bool dirty() const;
  void markDirty();

  // Typed accessors. Exactly one is non-null based on kind().
  Message*    as_message();
  Tool*       as_tool();
  Attachment* as_attachment();
};
```

Filters never construct `AiItem` directly; the runtime does.

### 4.6 `AiResponse` — envelope + variant summary

`codec/ai_response.h`. Applies the same envelope+variant pattern as
`AiRequest`. The variant holds **summary** scalars only (usage,
finish_reason, task status) — the actual response content lives in the
chunk stream (§4.8) because streaming is the common case and buffering
the whole response before running the chain would defeat the point.

```cpp
struct InferenceResponseSummary {
  std::string id;                 // response id echoed from backend
  std::string model;              // model actually used
  std::string finish_reason;      // "stop", "length", "tool_calls", "content_filter"
  struct Usage {
    absl::optional<int32_t> prompt_tokens;
    absl::optional<int32_t> completion_tokens;
    absl::optional<int32_t> total_tokens;
  } usage;
  absl::flat_hash_map<std::string, std::string> extra;
};

struct AgentResponseSummary {
  AgentDialect dialect{AgentDialect::Unknown};
  std::string  task_id;           // A2A
  std::string  task_status;       // "submitted", "working", "completed", "failed"
  std::string  error_code;        // JSON-RPC error code when applicable
  absl::flat_hash_map<std::string, std::string> extra;
};

class AiResponse {
public:
  // HTTP-level (populated at onResponseStart).
  uint32_t http_status{0};
  // Non-owning view of upstream response headers (owned by the
  // dispatch filter's AsyncStream). Filters may read/mutate;
  // ResponseEncoder re-emits downstream from this map. Optional
  // trailers handled the same way via `trailers` below if needed.
  Http::ResponseHeaderMap* headers{nullptr};

  // Correlates with the AiRequest that produced this response.
  std::string jsonrpc_id;
  ProtocolKind protocol{ProtocolKind::NonAi};
  std::variant<std::monostate, InferenceResponseSummary, AgentResponseSummary>
      summary;

  bool streaming{false};
  PayloadStore* payload_store{nullptr};
  AiScratch scratch;

  InferenceResponseSummary*       as_inference();
  const InferenceResponseSummary* as_inference() const;
  AgentResponseSummary*           as_agent();
  const AgentResponseSummary*     as_agent() const;
};
```

### 4.7 `AiResponseChunk` — materialized streaming chunk

`codec/ai_response_chunk.h`. Symmetric to `AiItem` on the request
side: a runtime-owned, materialized view that lives only for the
duration of one `onResponseChunk` call. For streaming responses, one
chunk per SSE event / delta. For non-streaming responses, a single
`Final` chunk carrying the whole body.

```cpp
enum class AiChunkKind {
  Started,          // response created: id, model, created-at
  ItemAdded,        // a new output item / choice appeared
  ContentDelta,     // text/content delta on an item
  ReasoningDelta,   // reasoning block delta (o1, Responses reasoning)
  ToolCallDelta,    // tool-call name / arguments delta
  ItemDone,         // an output item finished
  Completed,        // response done: finish_reason, usage
  ErrorEvent,       // upstream error event
  Final,            // non-streaming: whole body as one chunk
  Raw,              // protocol event the mapper didn't model
};

struct ContentDelta {
  PayloadRef  text;        // offloadable for large deltas / Final bodies
  std::string content_type;
};

struct ToolCallDelta {
  size_t      tool_call_index;
  std::string name_delta;       // incremental
  PayloadRef  arguments_delta;  // incremental JSON arguments
};

struct ItemAdded {
  std::string role;         // "assistant", "tool", etc.
  std::string output_type;  // "message", "tool_call", "reasoning", ...
};

struct Completed {
  std::string finish_reason;
  InferenceResponseSummary::Usage usage;
};

struct FinalBody {
  PayloadRef body;
};

class AiResponseChunk {
public:
  AiChunkKind kind() const;
  size_t      item_index() const;     // which output item this chunk belongs to
  bool        dirty() const;
  void        markDirty();

  // Typed accessors — populated based on kind().
  ContentDelta*   as_content_delta();
  ReasoningDelta* as_reasoning_delta();
  ToolCallDelta*  as_tool_call_delta();
  ItemAdded*      as_item_added();
  Completed*      as_completed();
  FinalBody*      as_final();
  // Started / ItemDone / ErrorEvent / Raw accessors likewise.
};
```

Filters never construct `AiResponseChunk` directly; the runtime does,
driven by protocol mappers that translate wire events (OpenAI SSE,
A2A task events, MCP notifications) into chunks.

## 5. Filter chain surface (`chain/`)

Operators should be able to write an `AiFilter` in a few dozen lines
without touching HTTP plumbing. That is the whole point of this filter.

### 5.1 `AiFilter` interface

`chain/ai_filter.h`. The chain runs in **phases**. A filter implements
only the phases it cares about; defaults are no-op `Continue`. This
keeps metadata-only filters (rate limit, budget, model routing) free
of any payload-I/O concerns, and lets the runtime skip materializing
large payloads when no filter in the chain needs them.

```cpp
enum class AiFilterStatus {
  Continue,        // advance to next filter (same phase)
  StopIteration,   // pause; resume via cb.continueRequest()
};

// Bitset: which item kinds this filter wants onRequestItem calls for.
struct AiItemKindSet {
  bool messages{false};
  bool tools{false};
  bool attachments{false};
  static AiItemKindSet all();
  static AiItemKindSet none();
};

// Bitset: which response chunk kinds this filter wants onResponseChunk
// calls for. Same skip-optimization pattern as AiItemKindSet.
struct AiChunkKindSet {
  bool started{false};
  bool item_added{false};
  bool content_delta{false};
  bool reasoning_delta{false};
  bool tool_call_delta{false};
  bool item_done{false};
  bool completed{false};
  bool error_event{false};
  bool final{false};
  bool raw{false};
  static AiChunkKindSet all();
  static AiChunkKindSet none();
};

class AiFilter {
public:
  virtual ~AiFilter() = default;

  // ======================== Request side ========================

  // Phase Q1: scalars only. Always invoked. Sees envelope + variant
  // payload's scalar fields. Does not trigger payload materialization.
  // Most cross-cutting filters stop here.
  virtual AiFilterStatus onRequestMetadata(AiRequest&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase Q2+: per-item, iterated across messages/tools/attachments.
  // Runtime materializes the item from PayloadStore before the call
  // and re-stores it on return if `item.dirty()`. Only invoked for
  // kinds this filter declared interest in via itemInterest().
  virtual AiItemKindSet itemInterest() const { return AiItemKindSet::none(); }
  virtual AiFilterStatus onRequestItem(AiItem&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // ======================== Response side =======================

  // Phase R1: upstream response headers arrived. Scalars only:
  // http_status, response id / model echoed back, early metadata.
  // Always invoked. No content materialization.
  virtual AiFilterStatus onResponseStart(AiResponse&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase R2: per-chunk, as chunks arrive from upstream. For streaming
  // responses: one call per SSE event / delta. For non-streaming
  // responses: one call with kind=Final carrying the whole body.
  // Filter declares interest via chunkInterest() — if no filter in
  // the chain is interested in a kind, chunks of that kind pass
  // through untouched without materialization.
  virtual AiChunkKindSet chunkInterest() const {
    return AiChunkKindSet::none();
  }
  virtual AiFilterStatus onResponseChunk(AiResponseChunk&,
                                         AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  // Phase R3: response complete. Final usage, finish_reason, trailers.
  // Scalars only. Always invoked after the chunk stream ends.
  virtual AiFilterStatus onResponseEnd(AiResponse&, AiFilterCallbacks&) {
    return AiFilterStatus::Continue;
  }

  virtual void onDestroy() {}
};
```

Assembled-item view on the response side (e.g. "give me the full
message once all deltas have arrived") is intentionally **not**
offered in v0. Filters that need complete messages, tool-calls, or
reasoning blocks for caching / schema validation / output moderation
buffer across `onResponseChunk` calls themselves. If the buffering
boilerplate proves common, a gated `onResponseItem` phase can be
added later. See open questions in §11.

Why one generic `onRequestItem` rather than typed
`onRequestMessage` / `onRequestTool` / `onRequestAttachment`: most
real filters (PII scrub, redaction, size caps, classification) treat
all large items uniformly; forcing three copies of the same logic is
worse than dispatching on `item.kind()` internally. Typed access
stays available via `item.as_message()` / `as_tool()` / `as_attachment()`.

### 5.2 `AiFilterCallbacks`

`chain/ai_filter_callbacks.h` — the only way an `AiFilter` interacts
with the world. Deliberately narrow:

```cpp
class AiFilterCallbacks {
public:
  virtual Event::Dispatcher& dispatcher() = 0;
  virtual StreamInfo::StreamInfo& streamInfo() = 0;
  virtual const AiProtocolManagerConfig& config() = 0;

  // Resume after StopIteration. Valid at whatever granularity the
  // pause happened (any request-side or response-side phase).
  virtual void continueRequest() = 0;
  virtual void continueResponse() = 0;

  // Short-circuit BEFORE dispatch: never talks to upstream. Synthesizes
  // a direct reply (e.g. guardrail denial on the request side).
  // Valid in any request-side phase.
  virtual void sendLocalReply(AiResponse&&) = 0;

  // Short-circuit DURING/AFTER dispatch: upstream is already engaged;
  // cut the in-flight response short and emit a synthetic tail
  // downstream. Valid in any response-side phase.
  virtual void endResponseEarly(AiResponse&&) = 0;

  // --- Per-item callbacks (valid only inside onRequestItem). ---
  virtual void dropCurrentItem() = 0;
  virtual void insertAfter(AiItem&&) = 0;

  // --- Per-chunk callbacks (valid only inside onResponseChunk). ---
  // Don't forward this chunk downstream.
  virtual void dropCurrentChunk() = 0;
  // Inject a chunk after the current one (flows through subsequent
  // filters, then downstream). Useful e.g. for splicing a synthetic
  // system message or a guardrail notice into the stream.
  virtual void insertAfter(AiResponseChunk&&) = 0;

  // Emit stats / access-log entries in the AI-manager namespace.
  virtual void recordEvent(AiEvent) = 0;
};
```

What is **intentionally not exposed** through callbacks: raw
`Buffer::Instance`, route config, cluster manager, direct filter-manager
plumbing. Headers *are* first-class — `AiRequest::headers` and
`AiResponse::headers` carry native `Http::RequestHeaderMap` /
`Http::ResponseHeaderMap` pointers — because HTTP headers are part of
the request model and wrapping them in a flat map would lose
case-insensitivity, inline slots, and multi-value support for no
benefit. The rule is "filters interact through the `AiRequest` /
`AiResponse` model, not through side-channel HTTP plumbing," not "no
Envoy HTTP types."

### 5.3 `AiFilterChain`

`chain/ai_filter_chain.h` holds an ordered `std::vector<AiFilterPtr>`
and runs the phased state machine. Single implementation used by both
sub-chains; the distinction is purely configuration.

**Phase-major ordering across filters.** Each phase completes across
the entire chain before the next begins:

```
Request side
  onRequestMetadata        : f1 → f2 → … → fN
  onRequestItem(msg 0)     : f1 → f2 → … → fN
  onRequestItem(msg 1)     : f1 → f2 → … → fN
  …
  onRequestItem(tool 0)    : f1 → f2 → … → fN
  …
  onRequestItem(attach 0)  : f1 → f2 → … → fN

(dispatch to upstream; response begins)

Response side
  onResponseStart          : f1 → f2 → … → fN
  onResponseChunk(0)       : f1 → f2 → … → fN     ← streaming: one pass per chunk
  onResponseChunk(1)       : f1 → f2 → … → fN
  …
  onResponseEnd            : f1 → f2 → … → fN
```

This mirrors the HTTP filter mental model (`decodeHeaders` for all
filters, then `decodeData` chunks for all filters) and lets the runtime
stream through large item lists and chunk streams holding only one
materialized `AiItem` / `AiResponseChunk` in memory at a time.

**Phase-skip optimization.** At chain-build time the runtime unions
the declared interests across all filters:

- **Request items** — union of `itemInterest()`. If no filter is
  interested in a kind (messages / tools / attachments), the runtime
  **skips the kind entirely** — items remain as `PayloadRef`s in the
  payload variant and are re-encoded by `RequestEncoder` without ever
  being materialized into filter memory.
- **Response chunks** — union of `chunkInterest()`. Chunks of
  unclaimed kinds pass through untouched to downstream without
  materialization. A chain full of metadata-only filters never
  materializes a single streaming delta.

This is the core I/O-hiding guarantee: a chain full of metadata-only
filters never touches `PayloadStore::fetch`, even when the underlying
payloads live in external storage.

**Mutation & re-emit.** After `onRequestItem` / `onResponseChunk`
returns, the runtime checks `.dirty()`. Dirty items are written back
to `PayloadStore` and the owning `PayloadRef` updated; dirty chunks
are re-serialized before forwarding downstream. Clean items/chunks
pass through by reference.

**Pause semantics.** `StopIteration` in any phase pauses the whole
chain at that point. `continueRequest()` / `continueResponse()`
resumes from the same filter and same item/chunk. Per-item and
per-chunk work is serialized — only one item/chunk is in flight at a
time — to keep the mental model simple. A slow filter caps end-to-end
streaming latency by construction; parallelism across chunks is a
later optimization.

**Early termination — two modes.** `sendLocalReply` (request-side
phases) never talks to upstream; the chain synthesizes a reply
directly. `endResponseEarly` (response-side phases) cuts an in-flight
upstream response short, emits a synthetic tail downstream (e.g. a
`Completed` chunk with `finish_reason=content_filter`), and tears
down the upstream stream. Both are terminal — subsequent phases on
their respective sides are not invoked.

### 5.4 `InferenceChain` / `AgenticChain`

`chain/inference_chain.h` and `chain/agentic_chain.h` are thin typed
façades over `AiFilterChain`. They exist so:

- Registration factories live in separate namespaces
  (`InferenceFilterFactoryRegistry`, `AgentFilterFactoryRegistry`) and
  can be searched independently.
- Future protocol-specific helpers (e.g. `InferenceChain::modelTarget()`
  accessor, `AgenticChain::sessionId()`) have a natural home without
  polluting the shared base.

### 5.5 Sub-chain configuration

Proto sketch (names only):

```
AiProtocolManager
├── inference_chain
│   ├── filters []       // repeated AiFilterConfig
│   └── dispatch         // InferenceDispatchConfig
├── agentic_chain
│   ├── filters []
│   └── dispatch         // AgenticDispatchConfig
├── dispatch_mode        // FALLOUT (default) | CHAIN_FORWARD
│                        // CHAIN_FORWARD: backend_cluster/routing fields
│                        //   in InferenceDispatchConfig /
│                        //   AgenticDispatchConfig are ignored (router owns it)
├── codec
│   ├── max_inline_bytes
│   ├── payload_store    // InMemory | FileApi { uri, creds, … }
│   └── protocol_override
└── classifier           // path prefixes, method allowlist, …
```

Each `AiFilterConfig` is `{ name, typed_config }` matching existing
Envoy idioms; factories register against
`Envoy::Registry::FactoryRegistry<Chain::AiFilterFactory>`.

## 6. Dispatch (`dispatch/`)

`AiDispatchFilter` is the tail of a sub-chain. It is **not** an
`AiFilter` — it sits outside the chain abstraction the way `router` sits
outside `http_filters`. The mode (fallout vs chain-forward) is a
per-instance configuration choice; both modes share the same
`RequestEncoder` step and the same `AiDispatchFilter` class hierarchy.

### 6.1 Shared responsibilities (both modes)

1. Invoke `RequestEncoder` to materialize the outbound HTTP request
   (method, path, headers, optional body) from the possibly-rewritten
   `AiRequest`. Must honor the §4.3 round-trip invariant.
2. Mutate `AiRequest::headers` and the body buffer in place so the
   re-encoded values are what propagates downstream (whether via
   `AsyncClient` in fallout mode or via `continueDecoding()` in
   chain-forward mode).

### 6.2 Fallout mode

The dispatch filter also owns the full upstream transport:

3. Resolve per-backend routing: one or N backends (fanout), which
   cluster / path / auth header set. For resource ops
   (`ResponsesRetrieve` etc.) routing is pinned to the backend that
   created the resource.
4. Open streams via `Http::AsyncClient` (reuse the
   `MuxDemux`/`MultiStream` primitives already used by `mcp_router`, see
   `source/extensions/filters/http/mcp_router/backend_stream.h`).
5. Feed upstream response headers / body / SSE events into
   `ResponseDecoder`, which produces `AiResponse` + a stream of
   `AiResponseChunk`s. The outer `AiProtocolManagerFilter` runs the
   sub-chain's response-side phases (§5.1) over those chunks and
   forwards the re-encoded output downstream via
   `decoder_callbacks_->encodeHeaders/Data/Trailers`.

### 6.3 Chain-forward mode

After step 2 the dispatch filter does **not** open an `AsyncClient`.
Instead:

3. Write the re-encoded method, path, and headers back into the
   downstream request header map already owned by
   `decoder_callbacks_`. If the body changed, drain the existing
   `Buffer::Instance` and append the new encoded body.
4. Call `decoder_callbacks_->continueDecoding()`. Control returns to
   the Envoy HTTP filter manager, which runs the remaining filter
   chain (e.g. `ext_authz`, `rate_limit`, `router`).
5. Upstream delivery and response buffering are handled entirely by the
   Envoy `router` filter using the route and cluster config already
   wired to this listener.
6. The upstream response arrives at `AiProtocolManagerFilter` via the
   encoder callbacks (`encodeHeaders`, `encodeData`, `encodeTrailers`).
   These feed `ResponseDecoder` → sub-chain response phases →
   `ResponseEncoder`, then call `encoder_callbacks_->continueEncoding()`
   to pass the mutated response downstream.

Because the response arrives on the encode side (not via an owned
`AsyncClient`), `AiProtocolManagerFilter` must implement both
`Http::StreamDecoderFilter` **and** `Http::StreamEncoderFilter`
interfaces. In fallout mode only the decoder interface is needed.

### 6.4 Subclass structure

`InferenceDispatchFilter` and `AgenticDispatchFilter` subclass
`AiDispatchFilter` and supply protocol-specific behaviour that is
independent of the dispatch mode:

- Backend selection strategy (model-based vs capability-based vs
  resource-pinned) — used only in fallout mode; chain-forward relies
  on route config.
- Response-wire mapping: the protocol-specific translator from
  upstream SSE / JSON / JSON-RPC events to `AiResponseChunk` kinds
  (OpenAI `chat.completions` chunk framing, OpenAI Responses typed
  events, A2A task events, MCP JSON-RPC result/notifications). This
  mapping is invoked from whichever response path delivers bytes
  (AsyncClient callbacks in fallout mode, `encodeData` in
  chain-forward mode).
- Error taxonomy mapping back into `AiResponse` / `ErrorEvent`
  chunks.

## 7. Request / response lifecycle

The request side is identical for both modes up to the dispatch step.

```
=============== Request side (both modes) ===============

decodeHeaders
  → RequestDecoder::onHeaders
     - populates verb/path/path_params/query_params/headers on AiRequest
     - runs classifier → ProtocolKind + (maybe) invocation → pick SubChain
  → install PayloadStore
  → if no body expected (bodiless verb): skip to SubChain dispatch
  → else: StopIteration, wait for body

decodeData (streaming, only if body present)
  → RequestDecoder::onData
  → large fields flushed to PayloadStore as External refs

decodeTrailers / end_stream
  → RequestDecoder::onEndStream → AiRequest
  → SubChain::runRequest(AiRequest)
       ├ Q1 onRequestMetadata      : f1 → f2 → …
       ├ Q2 onRequestItem(msg i)   : f1 → f2 → …   (only if any filter
       │                                            declared interest)
       ├ Q2' tools / attachments   : same pattern
       └ any filter may StopIteration → continueRequest()
         or sendLocalReply → synthesize response, skip dispatch
```

### 7.1 Fallout mode lifecycle

```
=============== Dispatch (fallout) =====================

DispatchFilter
  ├ RequestEncoder → {method, path, headers, body?}
  ├ Http::AsyncClient → upstream(s)
  └ wire up response callbacks → ResponseDecoder

=============== Response side (fallout) ================

upstream headers arrive (via AsyncClient callback)
  → ResponseDecoder::onHeaders → AiResponse (http_status, headers,
    protocol, summary scalars when known early)
  → SubChain::runResponse()
       └ R1 onResponseStart : f1 → f2 → …

upstream body / SSE events arrive (streaming)
  → ResponseDecoder::onData → one AiResponseChunk per event/delta
    (or one Final chunk for non-streaming bodies)
  → for each chunk:
       R2 onResponseChunk : f1 → f2 → …
         - chunks of kinds nobody declared interest in are passed
           through without materialization
         - dirty chunks are re-serialized by ResponseEncoder before
           forwarding downstream
         - any filter may StopIteration (halts the stream) or
           endResponseEarly (cuts upstream, emits synthetic tail)
  → ResponseEncoder → forward to downstream via decoder_callbacks_

upstream end_stream
  → ResponseDecoder::onEndStream → AiResponse summary finalized
  → R3 onResponseEnd : f1 → f2 → …
  → ResponseEncoder::finalize → downstream end_stream
```

### 7.2 Chain-forward mode lifecycle

```
=============== Dispatch (chain-forward) =====================

DispatchFilter
  ├ RequestEncoder → {method, path, headers, body?}
  ├ Mutate decoder_callbacks_ header map in place
  │   (method, path, content-type, content-length, etc.)
  ├ Replace body buffer via decoder_callbacks_
  └ decoder_callbacks_->continueDecoding()
       → Envoy filter manager runs remaining HTTP filters
       → Envoy router filter contacts upstream

=============== Response side (chain-forward) ================

upstream headers arrive (via Envoy filter manager → encodeHeaders)
  → ResponseDecoder::onHeaders → AiResponse (http_status, headers,
    protocol, summary scalars when known early)
  → SubChain::runResponse()
       └ R1 onResponseStart : f1 → f2 → …
  → mutate encoder_callbacks_ header map in place
  → encoder_callbacks_->continueEncoding() (or hold for body)

upstream body / SSE events arrive (via encodeData, one call per chunk)
  → ResponseDecoder::onData → one AiResponseChunk per event/delta
    (or one Final chunk for non-streaming bodies)
  → for each chunk:
       R2 onResponseChunk : f1 → f2 → …
         (same dirty/skip/stop semantics as fallout mode)
  → ResponseEncoder → inject re-serialized bytes into the encode Buffer
  → encoder_callbacks_->continueEncoding()

upstream end_stream (encodeTrailers or end_stream flag on encodeData)
  → ResponseDecoder::onEndStream → AiResponse summary finalized
  → R3 onResponseEnd : f1 → f2 → …
  → ResponseEncoder::finalize → encoder_callbacks_ end_stream
```

**Streaming in chain-forward mode.** Because `encodeData` may be called
many times (once per SSE frame from the router), `ResponseDecoder` runs
its SSE/JSON-RPC frame splitter incrementally across `encodeData` calls,
exactly as `RequestDecoder` does across `decodeData` calls. This means
`onResponseChunk` fires inside `encodeData` — one invocation per
complete frame, regardless of how TCP segments the bytes. The encode
buffer is drained (or replaced) on each `encodeData` return so the
router's backpressure signal naturally propagates upstream.

## 8. Stats & observability

`filter_config.h` defines the stat struct (pattern copied from
`McpRouterStats`):

```
AI_PROTOCOL_MANAGER_STATS(COUNTER)
  rq_total
  rq_inference
  rq_agent
  rq_classify_unknown
  rq_decode_error
  rq_encode_error
  rq_payload_offloaded
  rq_chain_stop
  rq_local_reply
  rq_dispatch_failure
```

Plus per-sub-chain histograms for decode/encode/dispatch latency.

## 9. Threading & lifetime

- Filter is per-stream, owned by the HTTP filter manager — same as
  `McpRouterFilter`.
- `PayloadStore` is per-filter by default; a pooled implementation
  (shared across streams on the same worker) is a later addition.
- `AiFilter` instances are per-stream and destroyed in `onDestroy()`
  along with the owning filter.
- No cross-worker state in v0.

## 10. Testing strategy (structural)

```
test/extensions/filters/http/ai_protocol_manager/
├── codec/
│   ├── request_decoder_test.cc
│   ├── request_encoder_test.cc
│   ├── response_decoder_test.cc
│   ├── response_encoder_test.cc
│   ├── codec_round_trip_test.cc     # invariant from §4.3
│   ├── json_rpc_parser_test.cc
│   ├── inference_mapping_test.cc
│   ├── agent_mapping_test.cc
│   └── payload_store_test.cc
├── chain/
│   ├── ai_filter_chain_test.cc
│   └── fake_ai_filter.h          # test helper
├── dispatch/
│   ├── inference_dispatch_test.cc
│   └── agent_dispatch_test.cc
├── filter_test.cc                # unit, with mock decoder/chain
├── integration/
│   ├── inference_integration_test.cc
│   └── agent_integration_test.cc
└── BUILD
```

A `fake_ai_filter.h` that records `onRequest` invocations is the
canonical way to write sub-chain tests; keeps the AI layer verifiable
without any HTTP spinup.

## 11. Open questions (to iterate on)

1. **Chain composition**: do we allow a single request to traverse
   both chains (e.g. agent invoking inference), or is that modeled
   as two separate requests? Current draft assumes the latter.
2. **Backpressure to offload**: what is the exact threshold policy —
   per-field byte limit, cumulative budget, or adaptive based on
   cluster memory pressure?
3. **Multi-backend SSE fan-in**: v0 dispatch is request/response or
   single-backend streaming. Merging N upstream SSE streams into one
   downstream stream (the `mcp_router` fan-in pattern) is deferred;
   the chunk / `AiResponseChunk` model is designed to accommodate it
   but the aggregation state machine isn't specified.
4. **Assembled-item view on the response side**: v0 offers only
   `onResponseChunk` (streaming deltas) and asks filters that need
   complete messages / tool-calls to buffer themselves. If the
   buffering boilerplate becomes common, add an `onResponseItem`
   phase gated by a declared interest flag, running after all deltas
   for an item have arrived.
5. **Auth / identity propagation**: do we reuse `mcp_router`'s
   `SubjectSource` abstraction verbatim, generalize it, or require
   upstream filters to populate `AiRequest::attributes`?
6. **Per-route overrides**: probable, modeled after `McpOverrideConfig`;
   not yet specified which fields are overridable per route.
7. **A2A vs MCP split**: `AgentPayload` currently unifies both
   dialects. Revisit if they diverge more than expected — likely
   fault line is the `AgentInvocation` enum growing unwieldy.
8. **`ReasoningDelta` as a distinct chunk kind**: kept separate from
   `ContentDelta` for strong typing. Alternative is a single
   `ContentDelta` with a `content_type` sub-field. Decide once there
   are real filters reading reasoning.
9. **`Final` chunk kind vs synthetic sequence**: non-streaming
   responses currently emit a single `Final` chunk. Alternative is
   to synthesize a `Started` → `ItemAdded` → `ContentDelta` →
   `ItemDone` → `Completed` sequence so filters see one model. More
   uniform but adds wire-translation cost on every non-streaming
   request.
10. **`sendLocalReply` / `endResponseEarly` in chain-forward mode**:
    in fallout mode these are straightforward because the dispatch
    filter owns the stream. In chain-forward mode `sendLocalReply`
    must suppress `continueDecoding()` and write a synthetic response
    via `decoder_callbacks_->sendLocalReply`, while `endResponseEarly`
    must drain the in-flight encode buffer and inject a synthetic
    tail — both require careful interaction with the Envoy filter
    manager's encode/decode state machine. Exact protocol TBD.
11. **`encodeHeaders` hold semantics in chain-forward mode**: to run
    `onResponseStart` and potentially mutate response headers before
    they reach downstream filters, `AiProtocolManagerFilter` must
    return `Http::FilterHeadersStatus::StopIteration` from
    `encodeHeaders` until the sub-chain phase completes. If the sub-chain
    is purely synchronous this is a single-tick hold; if a filter calls
    `StopIteration` the hold extends. The interaction with Envoy's
    watermark / flow-control on the encode side needs a prototype to
    confirm there are no deadlock paths.
