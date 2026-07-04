# Inference Filter Chain

The inference filter chain is the request/response processing pipeline for
OpenAI-style HTTP inference APIs inside `AiProtocolManagerFilter`. It provides
a structured interception point for chain filters (routing, PII scrubbing,
guardrails, cost budgeting, …) and, at dispatch time, translates the
(possibly mutated) request into the exact wire format required by the upstream
model provider.

---

## Architecture

```
downstream HTTP request (OpenAI format)
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│  AiProtocolManagerFilter                                        │
│                                                                 │
│  decodeHeaders / decodeData / decodeTrailers                    │
│          │                                                      │
│          ▼                                                      │
│  ┌───────────────────┐                                          │
│  │  RequestDecoder   │  parse body → InferencePayload          │
│  └────────┬──────────┘                                          │
│           │  AiRequest{protocol=Inference, payload=…}           │
│           ▼                                                      │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  InferenceChain (AiFilterChain)                         │   │
│  │                                                         │   │
│  │  Q1  onRequestMetadata  f0 → f1 → … → fN               │   │
│  │  Q2  onRequestItem(msg) f0 → f1 → … → fN  (per item)   │   │
│  │                                                         │   │
│  │  R1  onResponseStart    f0 → f1 → … → fN               │   │
│  │  R2  onResponseChunk    f0 → f1 → … → fN  (per chunk)  │   │
│  │  R3  onResponseEnd      f0 → f1 → … → fN               │   │
│  └─────────────────────────────────────────────────────────┘   │
│           │  Q1+Q2 complete                                      │
│           ▼                                                      │
│  ┌───────────────────┐                                          │
│  │  InferenceDispatch│  select encoder → mutate headers →      │
│  │                   │  inject body → continueDecoding()        │
│  └───────────────────┘                                          │
└─────────────────────────────────────────────────────────────────┘
         │  upstream HTTP request (provider native format)
         ▼
    Envoy router → model provider upstream
```

---

## Request decode (codec layer)

`RequestDecoder` translates the incoming HTTP request into a structured
`AiRequest` before the chain runs. For inference traffic the body is buffered
and parsed by `InferenceBodyParser` at end-of-stream.

**Classified paths:**

| HTTP method + path | Invocation | Body |
|---|---|---|
| `POST /v1/chat/completions` | `ChatCompletion` | yes |
| `POST /v1/completions` | `Completion` | yes |
| `POST /v1/responses` | `ResponsesCreate` | yes |
| `POST /v1/embeddings` | `Embeddings` | yes |
| `GET /v1/responses/{id}` | `ResponsesRetrieve` | no |
| `POST /v1/responses/{id}/cancel` | `ResponsesCancel` | no |
| `DELETE /v1/responses/{id}` | `ResponsesDelete` | no |
| `GET /v1/responses/{id}/input_items` | `ResponsesListInputItems` | no |

**Fields extracted into `InferencePayload`:**

| JSON field | Struct field | Notes |
|---|---|---|
| `model` | `target.name` | |
| `stream` | `request.streaming` | |
| `temperature` | `sampling.temperature` | |
| `top_p` | `sampling.top_p` | |
| `max_tokens` | `sampling.max_tokens` | |
| `n` | `sampling.n` | |
| `seed` | `sampling.seed` | |
| `stop` | `sampling.stop` | string coerced to `[string]` |
| `messages[]` | `messages` (PayloadRefs) | each element stored as `PayloadRef` |
| `tools[]` | `tools` (PayloadRefs) | each element stored as `PayloadRef` |
| *(everything else)* | `residual_params` | full body kept for round-trip |

**Provider hint:**

The `x-ai-provider` request header pins the upstream provider at decode time
and is stored in `target.provider_hint` (e.g. `"anthropic"`). Chain filters
may also write `provider_hint` directly. `InferenceDispatch` reads this at
chain exit to select the encoder.

---

## Chain phases

The inference chain runs the same phased state machine as the agentic chain.
Each registered `AiFilter` receives callbacks in phase-major order.

### Request phases (before upstream)

**Q1 — `onRequestMetadata`**

Fires once after the body is fully parsed. All scalar fields
(`target.name`, `sampling.*`, `streaming`, `path`, `http_method`,
`provider_hint`) are available. Typical uses: auth, routing rewrites,
model aliasing, rate-limit pre-check.

**Q2 — `onRequestItem`**

Fires once per element in `messages[]` and `tools[]`. Filters declare
which item kinds they care about via `AiFilter::itemInterests()`. Typical
uses: PII scrubbing on message content, tool definition validation, prompt
injection detection.

### Response phases (after upstream)

**R1 — `onResponseStart`** — upstream response headers received.

**R2 — `onResponseChunk`** — one call per SSE chunk or streamed body fragment.

**R3 — `onResponseEnd`** — upstream response complete.

### Pause / resume

A filter returning `StopIteration` from any phase pauses the chain at that
filter index. The chain resumes when the filter calls
`AiFilterCallbacks::continueRequest()` (or `continueResponse()`), exactly as
Envoy's `FilterManager::commonContinue()` works. A filter returning
`StopIteration` and never calling `continue` is equivalent to sending a local
reply that terminates the stream.

---

## Dispatch

`InferenceDispatch::dispatch()` is the tail of the inference chain. It runs
after Q1 and Q2 complete and performs four steps in order:

1. **Select encoder** — inspects `target.provider_hint` and calls the
   matching provider encoder. Falls back to the OpenAI round-trip encoder
   when no provider matches or the encoder returns `nullopt`.

2. **Mutate downstream headers** — writes `:method`, `:path`,
   `content-type`, and `content-length` into the Envoy header map via
   `callbacks.requestHeaders()`. The path may have been rewritten by a
   provider encoder (e.g. `/v1/messages` for Anthropic).

3. **Inject body** — calls `callbacks.addDecodedData()` to place the
   re-encoded body into the filter manager's buffer. Bodiless invocations
   (`GET`, `DELETE`) skip this step.

4. **Resume** — calls `callbacks.continueDecoding()` to hand the request
   to the next Envoy filter (typically the router).

---

## Provider encoders

Provider encoders translate the structured `InferencePayload` into the
wire format required by a specific upstream API. Each encoder is a pure
static function that takes `const AiRequest&` and returns
`absl::optional<RestHttpRequest>{method, path, body}`.

### `RequestEncoder::encodeInferenceBody` (default / OpenAI)

Round-trip re-encoder for OpenAI-compatible upstreams (OpenAI, Azure
OpenAI, vLLM, Mistral OpenAI-compat, etc.).

Strategy: seed from `residual_params` (the full original body) then
overlay extracted and potentially mutated scalar fields. `messages[]` and
`tools[]` are rebuilt from their current `PayloadRef` values so that per-item
mutations from Q2 are reflected.

Bodiless invocations (`ResponsesRetrieve`, `ResponsesCancel`,
`ResponsesDelete`, `ResponsesListInputItems`) return an empty string and
dispatch skips body injection.

### `AnthropicRequestEncoder::encode` (Anthropic)

Activated when `target.provider_hint == "anthropic"`.

**Field mapping:**

| OpenAI field | Anthropic field | Transformation |
|---|---|---|
| `model` | `model` | verbatim |
| `messages[role=system]` | `system` | extracted and concatenated; not in `messages[]` |
| `messages[role=user]` | `messages[]` | content blocks converted |
| `messages[role=assistant]` with `tool_calls` | `messages[]` | `tool_calls` → `tool_use` content blocks; `arguments` string parsed to object |
| `messages[role=tool]` | `messages[]` | consecutive runs merged into one `role:user` turn with `tool_result` blocks |
| `tools[].function.parameters` | `tools[].input_schema` | key renamed; JSON Schema unchanged |
| `tool_choice: "auto"` | `tool_choice: {"type":"auto"}` | string → object |
| `tool_choice: "none"` | `tool_choice: {"type":"none"}` | |
| `tool_choice: "required"` | `tool_choice: {"type":"any"}` | |
| `tool_choice: {type:function, function:{name}}` | `tool_choice: {type:tool, name}` | |
| `max_tokens` | `max_tokens` | required; defaults to `4096` if absent |
| `temperature` | `temperature` | verbatim |
| `top_p` | `top_p` | verbatim |
| `stop` (string \| array) | `stop_sequences` (array) | renamed; string coerced to array |
| `stream` | `stream` | verbatim |
| `n`, `seed` | *(dropped)* | not supported by Anthropic |
| `:path` | `/v1/messages` | rewritten by dispatch |

**Supported invocations:**

- `ChatCompletion` → full field translation to Anthropic Messages API
- `Completion` → `prompt` string wrapped as a `role:user` message

All other invocations return `nullopt`; dispatch falls back to the OpenAI
round-trip encoder.

### Adding a new provider encoder

1. Create `providers/<provider>/<provider>_request_encoder.{h,cc}` with a
   static `encode(const AiRequest&)` returning
   `absl::optional<RestHttpRequest>`.
2. Add a `BUILD` target for the new package (and a `providers/<provider>/BUILD`
   if it is a new intermediate directory).
3. In `dispatch/inference_dispatch.cc`, add a branch in the provider-selection
   block keyed on `target.provider_hint`.
4. Add the new library to `dispatch/BUILD`'s `inference_dispatch_lib` deps.

---

## Codec round-trip invariant

`RequestDecoder` + `RequestEncoder::encodeInferenceBody` form an inverse
pair for OpenAI-compatible upstreams:

```
decode(HTTP request) → AiRequest → encodeInferenceBody → HTTP request'
```

`HTTP request'` is semantically equivalent to the original: same method,
same path (absent chain-filter rewrites), same JSON payload (field order may
differ; `residual_params` preserves unknown fields).

Provider encoders break this invariant intentionally — they produce a
*different* wire format for a *different* upstream API.

---

## Directory structure

```
codec/
  request_decoder.cc      InferenceBodyParser — buffers and parses OpenAI body
  request_encoder.cc      encodeInferenceBody — OpenAI round-trip encoder

chain/
  inference_chain.h       InferenceChain typed façade + InferenceAiFilterFactory

dispatch/
  inference_dispatch.h    InferenceDispatch — provider routing + chain-forward dispatch
  inference_dispatch.cc

providers/
  anthropic/
    anthropic_request_encoder.h    OpenAI → Anthropic Messages API
    anthropic_request_encoder.cc

test/extensions/filters/http/ai_protocol_manager/
  anthropic_inference_integration_test.cc   E2E integration tests (17 cases)
```

---

## Writing an inference chain filter

Implement `AiFilter` (or `InferenceAiFilterFactory` for inference-only
filters) and register it with `InferenceAiFilterFactoryRegistry`.

```cpp
class MyInferenceFilter : public Chain::AiFilter {
public:
  // Q1: inspect / mutate scalars (model, path, sampling params, provider_hint).
  Chain::AiFilterStatus onRequestMetadata(Codec::AiRequest& req,
                                          Chain::AiFilterCallbacks& cbs) override {
    auto* p = req.as_inference();
    if (p && p->target.name == "gpt-4o") {
      p->target.name         = "claude-opus-4-5";
      p->target.provider_hint = "anthropic";
    }
    return Chain::AiFilterStatus::Continue;
  }

  // Q2: inspect / mutate individual messages or tool definitions.
  Chain::AiFilterStatus onRequestItem(Codec::AiItem& item,
                                      Chain::AiFilterCallbacks& cbs) override {
    // item.ref holds the PayloadRef for one message or tool.
    return Chain::AiFilterStatus::Continue;
  }

  Chain::AiItemKindSet itemInterests() const override {
    return {Codec::AiItemKind::Message};  // only receive message items
  }
};
```

The filter is registered in a factory and listed under `ai_filters` in the
`AiProtocolManager` proto config. Filters that apply to both inference and
agentic chains register in `AiFilterFactoryRegistry`; inference-only filters
register in `InferenceAiFilterFactoryRegistry`.
