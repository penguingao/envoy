# AI Protocol Manager — Filter Chain Architecture & Extension Guide

Companion to `DESIGN.md`. `DESIGN.md` specifies the filter; this file explains
how the pieces compose at runtime and where to hook in when adding a new
backend, protocol, or cross-cutting concern.

## 1. Runtime shape

```
downstream HTTP request
      │
      ▼
┌──────────────────────────────────────────────────────────────────────┐
│ AiProtocolManagerFilter  (outer terminal HTTP filter)                │
│   decodeHeaders → classify(path, content-type)                       │
│   decodeData    → AiRequestDecoder::onData (streaming)               │
│   decodeTrailers/end_stream → AiRequestDecoder::take() → AiRequest   │
│                                                                      │
│                           AiRequest (neutral)                        │
│            envelope fields + std::variant<Inference, Agent>          │
│                               │                                      │
│          ┌────────────────────┴────────────────────┐                 │
│          ▼                                         ▼                 │
│   InferenceChain                               AgentChain            │
│   (ordered AiFilter[]                          (ordered AiFilter[]   │
│    over AiRequest&)                             over AiRequest&)     │
│          │                                         │                 │
│          ▼                                         ▼                 │
│   InferenceDispatch                            AgentDispatch         │
│   ├─ pick AiRequestEncoder by target schema    ├─ JSON-RPC encoder   │
│   │    (OpenAiEncoder | GeminiEncoder | …)     ├─ backend selection  │
│   ├─ build path/host/auth per backend          └─ Http::AsyncClient  │
│   └─ Http::AsyncClient → upstream(s)                                 │
└──────────────────────────────────────────────────────────────────────┘
```

The **core contract** is `AiRequest`: a shared envelope (`jsonrpc_id`,
`method`, `protocol`, `attributes`, `streaming`, `scratch`, `payload_store`)
plus a `std::variant` holding `InferencePayload` (chat, system_instructions,
tools, tool_choice, response_format, sampling, target) or `AgentPayload`
(dialect, invocation, target, parts, arguments, …). Cross-cutting filters
take `AiRequest&` and never see the variant; backend-specific code (encoders,
dispatch) pulls out the variant it needs via `std::visit` or
`asInference()` / `asAgent()`.

Two decouplings do the heavy lifting:

1. **Decoder / Encoder asymmetry** (`codec/`). Decoders map wire format →
   neutral `AiRequest`; encoders go the other way. They are picked
   independently — decode as OpenAI, encode as Gemini.
2. **AiFilter vs HTTP filter** (`chain/ai_filter.h`). `AiFilter` is phased
   (`onRequestMetadata` scalars → `onRequestItem` per materialized
   message / tool / attachment → `onResponse`). Interest is declared via
   `itemInterest()` so metadata-only filters never trigger
   `PayloadStore::fetch` — that is the I/O-hiding guarantee.
   `AiFilterCallbacks` deliberately exposes zero HTTP types — no headers,
   no buffers, no cluster manager.

## 2. Extension axes

Ordered by how often they are touched:

| I want to… | What I edit | Example from current code |
|---|---|---|
| **Add a target backend** (Anthropic, Bedrock, Cohere, …) | New `AiRequestEncoder` subclass + new `TargetSchema` enum in `InferenceDispatch` proto + branch in `finalizeRequest` / `sendUpstream` | `GeminiEncoder` vs `OpenAiEncoder`; Vertex path template in `filter.cc` |
| **Add a source protocol** (Anthropic Messages in, Responses API, embeddings, …) | New classifier branch + new parser function that populates `AiRequest` | `parseOpenAIInferenceRequest` is the template; add `parseAnthropicMessagesRequest` next to it |
| **Add cross-cutting logic** (PII scrub, budget, rate-limit, caching, logging) | Implement `AiFilter`, register in the chain factory; never touch codec / dispatch | empty today — first real filter lands next phase |
| **Add an agent protocol** (A2A streaming, new MCP verbs) | Extend `AgentInvocation` enum + add mapping in `agent_mapping.cc` + dispatch encoder | sketched in DESIGN §4.1 agent variant |
| **Add payload offload** (GCS, S3, FileAPI) | Implement `PayloadStore` interface | `InMemoryPayloadStore` is the reference; `fetch()` can be async |

## 3. Walkthrough — adding Anthropic as a target backend

1. `codec/anthropic_encoder.{h,cc}` — subclass `AiRequestEncoder`. Map
   `InferencePayload::chat` to `messages[]` (Anthropic alternates
   user/assistant natively — no buffer-and-flush like Gemini).
   `system_instructions` → top-level `system` string. `tools` →
   `tools[]` with `input_schema` (the JSON-Schema string the parser
   already stashed as `ToolFunction::parameters_json`). `tool_choice` →
   `{type: "auto"|"any"|"tool", name}`.
2. `api/.../ai_protocol_manager.proto` — add `ANTHROPIC = 2` to
   `TargetSchema`.
3. `filter.cc` — add the case in the encoder switch; if Anthropic needs a
   custom URL, add a branch in `sendUpstream` mirroring the Vertex one.

Nothing else moves. The parser, chain, `AiFilter`s, stats, and outer
filter all stay put — that is the whole point of the neutral `AiRequest`.

## 4. Walkthrough — adding a PII-scrub filter

1. `chain/pii_scrub_filter.{h,cc}` —
   `class PiiScrubFilter : public AiFilter`. Declare
   `itemInterest() { return {.messages = true}; }`. In
   `onRequestItem`, inspect `item.as_message()->text`, rewrite, call
   `item.markDirty()` if you changed it. The runtime handles re-store.
2. Register a factory under `Chain::AiFilterFactory` so config can name
   it.
3. Add to `inference_chain.filters[]` in the HCM config.

No decoder, encoder, or outer filter changes. And because the filter
never sees HTTP primitives, it is trivially unit-testable against
synthetic `AiRequest`s.

## 5. What the architecture intentionally makes hard

- Filters cannot read raw headers or add HTTP buffers — forces
  structured data to live on `AiRequest::attributes` (observable by
  every filter) rather than sneak through headers.
- No filter can opaque-passthrough a payload it mutated —
  `markDirty()` + re-store is the only path, which keeps the encoder's
  view authoritative.
- The variant is closed (`Inference`, `Agent`). Adding a third payload
  kind (e.g. fine-tuning jobs) requires touching every `std::visit` —
  on purpose, so no codepath silently drops unknown kinds.

## 6. Known gap — response path

V0 dispatch is buffer-then-reply. Streaming (SSE for OpenAI, Gemini
`streamGenerateContent`, A2A events) needs a symmetric `AiResponse` +
response-side `AiFilter` phase — sketched in `DESIGN.md` §11 and
partly expanded in the response-side design additions upstream.
