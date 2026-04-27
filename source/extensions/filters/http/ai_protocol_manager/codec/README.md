# Codec — RequestDecoder and RequestEncoder

`RequestDecoder` and `RequestEncoder` are the two halves of the codec layer.
`RequestDecoder` converts an incoming HTTP request into a structured `AiRequest`;
`RequestEncoder` converts the (possibly mutated) `AiRequest` back into an outbound
HTTP request. They are inverses of each other. A third path — the **REST transcoder**
— lets `RequestEncoder` lower an agentic request to a plain REST call instead of
re-emitting JSON-RPC.

---

# RequestDecoder

`RequestDecoder` translates an incoming HTTP request (headers + streamed body chunks)
into a fully-populated `AiRequest`. It is owned by `AiProtocolManagerFilter` — one
instance per stream — and is driven by the filter's `decodeHeaders` / `decodeData` /
`decodeTrailers` callbacks.

## State machine

```
AwaitingHeaders
    │  onHeaders() — classify, pick parser
    ├─► BodilessComplete   (GET/DELETE, or POST with no body hint)
    │       │  onEndStream()
    │       └─► BodyComplete ──► take()
    │
    ├─► ParsingInferenceBody   (POST /v1/chat/completions, …)
    │       │  onData() * N
    │       │  onEndStream()
    │       └─► BodyComplete ──► take()
    │
    └─► ParsingAgentBody       (POST application/json, JSON-RPC heuristic)
            │  onData() * N
            │  onEndStream()
            └─► BodyComplete ──► take()
```

Any step that fails moves to `Error`; `take()` returns `FailedPrecondition` from that
state.

## Phases

### 1. `onHeaders` — classify and select parser

- Copies `:method`, `:path`, `headers*` onto the nascent `AiRequest`.
- Parses query parameters from the path string.
- Calls `classify()` with `rpc_method = ""` (body not yet seen). The classifier
  matches on HTTP method + path pattern to determine `ProtocolKind` and, when
  determinable from the path alone, the specific `InferenceInvocation` or
  `AgentInvocation`.
- **Bodiless** (`GET`, `DELETE`, or any verb with no `Content-Length` / `Transfer-Encoding`):
  populates the payload variant directly from the classify result and transitions to
  `BodilessComplete`. No body parser is created.
- **Inference** (`POST /v1/…` matched as `ProtocolKind::Inference`): creates
  `InferenceBodyParser` and transitions to `ParsingInferenceBody`.
- **Agentic** (`ProtocolKind::AgenticMcp` or `AgenticA2a`): creates `AgentBodyParser`
  and transitions to `ParsingAgentBody`. Final invocation is not yet known — it will be
  resolved from the JSON-RPC `"method"` token in the body.

### 2. `onData` — stream body chunks

Forwards each chunk to whichever inner parser is active. No-op in all other states.

- **`InferenceBodyParser`**: appends the chunk to an in-memory buffer; no
  incremental processing.
- **`AgentBodyParser`**: feeds the chunk to `McpJsonParser` for incremental
  streaming SAX parsing with early-stop when all required fields are collected.

### 3. `onEndStream` — finalize

- **`BodilessComplete`**: no-op; advances to `BodyComplete`.
- **`ParsingInferenceBody`**: calls `InferenceBodyParser::finish()`, which parses
  the buffered body with `Json::Factory::loadFromString` and populates
  `InferencePayload`:
  - Scalar fields: `model`, `stream`, sampling params (`temperature`, `top_p`,
    `max_tokens`, `n`, `seed`, `stop`).
  - Array fields: `messages` and `tools` — each element serialized back to JSON
    and stored as a `PayloadRef`.
  - `residual_params`: the full raw body, stored for round-trip pass-through.
- **`ParsingAgentBody`**: calls `AgentBodyParser::finish()`, which:
  1. Calls `McpJsonParser::finishParse()` to flush the streaming parser.
  2. Extracts `"id"` (string or number) → `request.jsonrpc_id`.
  3. Extracts `"method"` → `request.rpc_method`, then **re-classifies** with
     `rpc_method` populated to resolve the specific `AgentInvocation` and finalize
     `ProtocolKind`.
  4. Populates invocation-specific `AgentPayload` fields from the parsed
     `Protobuf::Struct` metadata:

     | Invocation | Fields populated |
     |---|---|
     | `ToolsCall` | `tool_name` ← `params.name`; `arguments` ← `params.arguments` |
     | `ResourcesRead/Subscribe/Unsubscribe` | `resource_uri` ← `params.uri` |
     | `PromptsGet` | `prompt_name` ← `params.name`; `arguments` ← `params.arguments` |
     | `CompletionComplete` | `completion_ref` ← `params.ref` |
     | `Initialize` | `capabilities` ← `params.capabilities` |

  5. Stores `params_raw` (full `"params"` value as JSON) and `residual_params`
     (whole parsed metadata) for round-trip fidelity.

### 4. `take` — move `AiRequest` out

Valid only after a successful `onEndStream()`. Moves the completed `AiRequest` to the
caller and resets the decoder back to `AwaitingHeaders` so it can be reused for the
next request (one decoder per filter instance is typical).

## Inner parsers

### `InferenceBodyParser`

Accumulates the full body in a `std::string` buffer. At `finish()` time it parses the
complete JSON in one shot using `Json::Factory::loadFromString`. This keeps the
implementation simple at the cost of holding the entire body in memory; large messages
and tool definitions are offloaded to `PayloadStore` as `PayloadRef`s so they are not
duplicated when re-encoded.

### `AgentBodyParser`

Wraps `McpJsonParser` (the same streaming SAX extractor used by `mcp_filter`) with an
extraction config that registers:
- Always: `jsonrpc`, `id`, `method`
- For `tools/call`: `params.name`, `params.arguments`
- For `prompts/get`: `params.name`, `params.arguments`

The parser stops as soon as all registered fields for the detected method have been
seen, avoiding a full parse of potentially large `arguments` objects. The extracted
fields are delivered as a `Protobuf::Struct` and then mapped into typed `AgentPayload`
fields by `populateFromMeta()`.

## Two-pass classification

For agentic requests, classification happens twice:

1. **Header-only** (`onHeaders`): HTTP method + path gives a coarse `ProtocolKind`
   (e.g. `AgenticMcp`) but `AgentInvocation` is `Unknown` because the JSON-RPC
   `"method"` field is in the body.
2. **Body-refined** (`AgentBodyParser::finish`): once `"method"` is seen in the body,
   `classify()` is called again with `rpc_method` set. This resolves the specific
   `AgentInvocation` (e.g. `ToolsCall`, `ResourcesRead`) and updates
   `request_.protocol` in place.

## `PayloadStore` and `PayloadRef`

Large field values (message arrays, tool definitions, `arguments` objects) are not
copied into the `AiRequest` directly. Instead, `RequestDecoder` calls
`PayloadStore::store()` which returns a `PayloadRef` — a lightweight handle that
either wraps an inline string (below `DecoderConfig::max_inline_bytes`, default 4 KB)
or an index into an off-request buffer. `AiFilter`s read these via `PayloadRef`;
`RequestEncoder` reads them back when re-encoding the outgoing request.

---

# RequestEncoder

`RequestEncoder` is the inverse of `RequestDecoder`. It is called by `AgenticDispatch`
after all chain phases (Q1, Q2) have completed, to serialize the (possibly mutated)
`AiRequest` back into an outbound HTTP request body. It exposes two static methods
covering two dispatch paths.

## `encodeAgentBody` — JSON-RPC re-encoding

Produces the JSON-RPC 2.0 wire body:

```json
{"jsonrpc":"2.0","id":"<id>","method":"<rpc_method>","params":<params>}
```

The `"id"` field is omitted when `jsonrpc_id` is empty (notification). The `"params"`
object is built in one of two ways depending on the invocation:

**Category A — fully-structured** (params are rebuilt from `AgentPayload` fields):

| Invocation | Encoded params |
|---|---|
| `ToolsCall` | `{"name":"<tool_name>","arguments":<arguments>}` |
| `ResourcesRead / Subscribe / Unsubscribe` | `{"uri":"<resource_uri>"}` |
| `PromptsGet` | `{"name":"<prompt_name>","arguments":<arguments>}` |

Because the params are reconstructed from the structured fields, any mutation a chain
filter made to `tool_name`, `resource_uri`, `prompt_name`, or `arguments` is
automatically reflected in the outgoing body — no re-parsing required.

**Category B — pass-through** (all other invocations: `Initialize`, `Ping`, list
operations, `CompletionComplete`, `LoggingSetLevel`, `SamplingCreateMessage`, all A2A
operations):

`params_raw` (the original `"params"` JSON captured verbatim by `AgentBodyParser`) is
inserted as-is. This guarantees a faithful round-trip for fields the decoder did not
extract into structured form (e.g. `protocolVersion`, `clientInfo`, cursor tokens,
A2A message parts). Mutations to `AgentPayload` fields beyond the Category A set are
**not** reflected in the body for these invocations.

## `encodeAgentBodyAsRest` — REST transcoding ⬡

> **This is the transcoder path.** When a `McpRestTranscoderRouteConfig` is present on
> the matched route (resolved by `AgenticDispatch` via
> `resolveMostSpecificPerFilterConfig`), the encoder lowers the agentic request to a
> plain REST HTTP call instead of re-emitting JSON-RPC. This allows an MCP client to
> talk JSON-RPC while the upstream sees a conventional REST API — no changes to either
> side.

The transcoder is configured per-route in `typed_per_filter_config` and maps MCP
invocations to `HttpRule` entries (Google HTTP annotation style):

```yaml
typed_per_filter_config:
  envoy.filters.http.ai_protocol_manager:
    "@type": type.googleapis.com/...McpRestTranscoderConfig
    tools:
    - tool_name: "search"
      rule:
        get: "/api/search/{query}"
    - tool_name: "create_doc"
      rule:
        post: "/api/docs"
        body: "*"
    tools_list:
      rule:
        get: "/api/tools"
    resources_read:
      rule:
        get: "/api/resources/{uri}"
```

**Supported invocations:** `ToolsCall` (per tool name), `ToolsList`, `ResourcesList`,
`ResourcesRead`. All others return `nullopt` and fall back to JSON-RPC re-encoding.

**How `buildRestRequest` works:**

1. **HTTP method and path pattern** — selected from the `HttpRule` field that is set
   (`get`, `put`, `post`, `delete`, `patch`).

2. **Path template substitution** — template variables (`{variable}` or
   `{variable=pattern}`) are extracted with a regex, resolved from the arguments JSON
   at the matching dot-path, percent-encoded, and substituted into the URL.
   - For `ToolsCall`: variables are resolved from `payload.arguments` (the decoded
     JSON object).
   - For `ResourcesRead`: `{uri}` is resolved from `payload.resource_uri` directly.

3. **Query parameters** — argument fields not consumed by a path template and not
   designated as the body (`rule.body`) are serialized as `key=value` query
   parameters appended to the URL.

4. **Request body** — controlled by `rule.body`:
   - `"*"` → the entire arguments object (minus path-template fields) becomes the
     body.
   - A dot-path string → that specific field of the arguments becomes the body.
   - Empty → no body.

Returns `nullopt` when no matching rule exists for the invocation/tool, when
`arguments` JSON is malformed, or when the `HttpRule` has no HTTP method set.
`AgenticDispatch` falls back to `encodeAgentBody` (JSON-RPC) in that case.

## Mutation visibility summary

| Field | Category A (structured) | Category B (pass-through) |
|---|---|---|
| `tool_name` | Reflected | Not reflected |
| `resource_uri` | Reflected | Not reflected |
| `prompt_name` | Reflected | Not reflected |
| `arguments` | Reflected | Not reflected |
| All other `params.*` | Not extracted; replayed from `params_raw` | Replayed from `params_raw` |
