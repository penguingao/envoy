What are the two options and the trade-off between them.

Option A — selectStringTarget + onStringComplete

The cursor owns accumulation. The handler returns a buffer pointer once; the cursor fills it; the handler reads it when done.


// InferenceBodyParser — routing on model + extracting stop strings
std::string* selectStringTarget(absl::string_view key, int depth, size_t /*tok_start*/) override {
    if (depth == 1) {
        if (key == "model") { model_.clear(); return &model_; }
        if (key == "stop")  { stop_val_.clear(); return &stop_val_; }
    }
    return nullptr;  // all other strings: zero allocation, zero callbacks
}

void onStringComplete(std::string* target, absl::string_view /*key*/,
                      int /*depth*/, size_t /*tok_end*/) override {
    if (target == &stop_val_) sampling_.stop.push_back(stop_val_);
    // model_ is already in place — nothing else to do
}
For a large request body like:


{"model": "gpt-4", "messages": [{"role": "user", "content": "10,000 word essay..."}]}
selectStringTarget("content", 3) returns nullptr. The cursor fires zero further calls for that 10,000-word string. No allocation, no callback overhead.

Option B — onStringChunk

Every string segment fires a callback regardless of interest. Handler must accumulate, dispatch, and track state itself.


absl::Status onStringChunk(absl::string_view key, int depth,
                           absl::string_view chunk, bool end,
                           size_t /*tok_start*/) override {
    if (depth == 1 && (key == "model" || key == "stop")) {
        pending_str_.append(chunk);    // ← handler doing cursor's old job
        if (end) {
            if (key == "model") model_ = std::move(pending_str_);
            else                sampling_.stop.push_back(pending_str_);
            pending_str_.clear();
        }
    }
    // else: receive every chunk of every string in the document and discard
    return absl::OkStatus();
}
For the same 10,000-word content field, the handler receives one callback per Wuffs token (~256 bytes each) — roughly 40 callbacks it immediately discards. Multiply by every message in the array.

Config-driven extraction — where the gap widens

InferenceBodyParser also extracts arbitrary string fields at runtime via extract_fields config. It calls cursor_.buildPaths() to pattern-match the current position.

With Option A, buildPaths is called once per string in selectStringTarget:


std::string* selectStringTarget(absl::string_view key, int depth, size_t /*tok_start*/) override {
    if (depth >= min_extract_depth_) {
        cursor_.buildPaths(depth, indexed_path_, pattern_path_);
        if (config_.patterns.contains(pattern_path_)) {
            scratch_.clear();
            return &scratch_;   // cursor fills it, onStringComplete acts on it
        }
    }
    return nullptr;
}
With Option B, the handler must decide whether to accumulate on the first chunk, then keep accumulating, then act on end=true — but what if the string fits in one chunk with end=true immediately? Need to handle both in the same call:


absl::Status onStringChunk(absl::string_view key, int depth,
                           absl::string_view chunk, bool end, size_t /*tok_start*/) override {
    if (!accumulating_) {
        if (depth >= min_extract_depth_) {
            cursor_.buildPaths(depth, indexed_path_, pattern_path_);
            if (config_.patterns.contains(pattern_path_)) {
                scratch_.clear();
                scratch_.append(chunk);
                if (end) { extracted_.emplace_back(indexed_path_, scratch_); return absl::OkStatus(); }
                accumulating_ = true;
                return absl::OkStatus();
            }
        }
        // discard — but chunk already arrived
    } else {
        scratch_.append(chunk);
        if (end) {
            extracted_.emplace_back(std::move(indexed_path_saved_), std::move(scratch_));
            accumulating_ = false;
        }
    }
    return absl::OkStatus();
}
Compared to Option A's clean selectStringTarget dispatch, this is a hand-rolled state machine that every handler must independently implement correctly.

Summary

Option A	Option B
Accumulation work	Cursor (once, shared)	Every handler (duplicated)
Discard cost	Zero — nullptr skips all further work	Receive all chunks, check and ignore
Key dispatch cost	Once per string	Once per chunk
buildPaths calls	Once per string	Once per string (but state machine needed)
Large unwanted strings	Zero overhead	N callbacks discarded
Byte range extraction	Via tok_start/tok_end params	Via tok_start + nextSrcPos()
Reviewer concern	std::string* looks unusual	Accumulation burden
The std::string* concern is real but it is a documentation problem — one clear comment explaining "handler provides storage, cursor writes to it, return nullptr to discard" resolves it. The accumulation burden of Option B is a design problem — every consumer must independently implement the same accumulation pattern and get the state machine right.

Verdict: Option A is the right design. Revert onStringChunk and restore selectStringTarget + onStringComplete, adding tok_start to selectStringTarget and tok_end to onStringComplete to address reviewer comment #4.



Take this inference request:


{
  "model": "gpt-4",
  "messages": [{"role": "user", "content": "Write me a 10000 word essay..."}]
}
Say your handler only wants model — it needs to route to the right backend. It doesn't care about content at all.

When "model" string starts:


selectStringTarget(key="model", depth=1, ...) → returns &model_buf_
str_target_ = &model_buf_. The cursor appends "gpt-4" into it. onStringComplete fires. Done.

When "content" string starts (the 10 000-word essay):


selectStringTarget(key="content", depth=3, ...) → returns nullptr
str_target_ = nullptr.

Wuffs now emits dozens of STRING tokens (the essay text, split across however many ring-buffer fills it takes). For each one:


if (str_target_ && tlen > 0) appendStringToken(...)
//   ↑ false — skip
Then it hits escape sequences like \n, \t — UNICODE_CODE_POINT tokens:


if (str_target_) appendCodePoint(...)
//   ↑ false — skip
All 10 000 words, every token, every escape: single branch, not-taken, nothing written, no allocation, no copy.

Without nullptr you'd std::string::append() tens of kilobytes into a buffer you immediately throw away. With nullptr the cost is the branch predicting false a few dozen times.


if not-taken, nothing written, no allocation, no copy, how is this string content added to final result
It isn't — and that's the point.

WuffsJsonCursor is a parser, not a serializer. It doesn't build the final response. It reads the incoming request and fires callbacks so your handler can extract the fields it cares about.

For the routing use case, the handler only needed model to decide which backend to forward to. The full request body — including the 10 000-word content — is sitting untouched in the original HTTP buffer. The cursor never needed to copy it anywhere.

When the handler forwards the request to the backend, it sends the original raw bytes, not anything the cursor built. The cursor's job was just to peek at "model": "gpt-4" so the handler knew where to send it.

So the flow is:


HTTP request bytes arrive
       │
       ├─→  WuffsJsonCursor reads them, fires callbacks
       │          └─→ handler sees model="gpt-4", picks backend
       │
       └─→  original bytes forwarded as-is to backend
The cursor is a reader on top of the stream. str_target_ = nullptr just means "I looked at this string and decided I don't need to remember its content" — the string still exists in the original buffer, the cursor just chose not to copy it.


The body is not forwarded directly — it's re-encoded. Here's the full path:

Incoming request bytes
        │
        ▼
  AiProtocolManagerFilter::decodeData()
        │  feeds chunks into
        ▼
  InferenceBodyParser  (WuffsJsonCursor inside)
        │  extracts: model, messages[], tools[], params
        │  content field → str_target_=nullptr, never copied
        ▼
  AiRequest (structured object with captured fields)
        │
        ▼
  InferenceDispatch::dispatch()
        │  re-encodes AiRequest back to JSON
        │  (potentially translated to a different provider's format)
        ▼
  callbacks.addDecodedData(new_body)  ← new buffer injected
  callbacks.continueDecoding()        ← forwarded upstream

The original body bytes are consumed and discarded. The cursor parses them into an AiRequest struct, and the dispatch layer re-serializes that struct into a new JSON body — potentially in a different provider's format (e.g., incoming OpenAI format translated to Anthropic format).

So when content gets str_target_ = nullptr, it means: "I don't need to copy the content text during parsing — but when I re-encode the AiRequest for upstream, the content is there because it was captured as a byte-range slice of the original buffer (tok_start / tok_end from onContainerOpen / onContainerClose), not a copied string." The cursor records where in the buffer the content lives, and the encoder emits those bytes directly at dispatch time.





Now the full decode path is clear. Here's exactly what happens:

During parsing (feed() → cursor callbacks):

selectStringTarget("model", depth=1) → returns &model_ — cursor writes "gpt-4" into it
selectStringTarget("content", depth=3) → returns nullptr — cursor skips all those bytes
onContainerOpen at depth 3 inside messages[] → records elem_start_ = tok_start
onContainerClose at depth 3 → records (elem_start_, tok_end) into message_ranges_
In finish():


// converts byte ranges to PayloadRef — slices of the original body buffer
makeSubRef(message_ranges_)  →  PayloadRef { start, end }
makeSubRef(tool_ranges_)     →  PayloadRef { start, end }
PayloadRef is just a pointer + length into the original body buffer (residual_writer_). No copy of content ever happens — the 10 000-word essay stays where it landed.

Forwarding upstream (request_encoder.cc::encodeInferenceBody):


// Step 1: parse residual_params (top-level fields the cursor didn't extract)
json body = json::parse(payload->residual_params.toString());
// Step 2: overlay extracted scalars
body["model"] = payload->target.name;   // ← what cursor captured into model_
body["stream"] = request.streaming;
// Step 3: splice message/tool byte-ranges directly back in
//         → slices of the original buffer, no re-copy of content
return body.dump();
So content is never copied — the PayloadRef byte-range points back into the original buffer, and encodeInferenceBody emits those bytes verbatim into the upstream body.


The content bytes are in residual_writer_ — the full raw body buffer that feed() builds:


// InferenceBodyParser::feed()
residual_writer_.append(chunk)   // every incoming chunk appended here
cursor_.feed(chunk, false)       // cursor reads same bytes, fires callbacks
Both happen on the same chunk. The cursor reads bytes for parsing; residual_writer_ stores those same bytes verbatim.

So by the time finish() runs, residual_writer_ holds the complete original request body — including the 10 000-word content — untouched.

message_ranges_ holds pairs like {1234, 5678} — byte offsets into that buffer marking where each message object starts and ends. makeSubRef(message_ranges_) turns those into PayloadRef objects that are just views into residual_writer_'s memory.

When encodeInferenceBody later splices messages[] back into the upstream body, it reads those PayloadRef slices — which point directly into residual_writer_ — and emits the bytes. The content text was always sitting there in residual_writer_. It was never parsed, never copied into a std::string, never touched by the cursor at all. It just got carried along as raw bytes from the moment feed() first saw it.


how does it work with mmap payload

Now the complete picture:

residual_writer_ writes directly into mmap:


feed(chunk)
  → residual_writer_.append(chunk)     // memcpy into mmap file region
  → cursor_.feed(chunk)                // parse same bytes, fire callbacks
Every incoming chunk lands in the mmap file via memcpy. The mmap region grows via ftruncate + remap as needed.

message_ranges_ records offsets into that mmap region:


onContainerOpen  → elem_start_ = tok_start   // byte offset in mmap
onContainerClose → message_ranges_.push({elem_start_, tok_end})
These are offsets into the mmap file, not pointers into heap memory.

makeSubRef turns those offsets into a PayloadRef:


PayloadRef::makeExternal(offset, length)
  → just stores {offset, length} — no copy
fetch() at dispatch time:


fetch(PayloadRef) → buf->add(map_ + offset, length)
One memcpy at the very end when the upstream body is being assembled.

So the content text is touched exactly twice:

feed() → memcpy into mmap (from Envoy's network buffer into the file)
fetch() → memcpy out of mmap into the upstream send buffer
Everything in between — the WuffsJsonCursor parsing, the message_ranges_ recording, the PayloadRef passing around — touches zero bytes of the content. Just integers (offset, length).

Yes. Three actually, based on what the header described:

Inline — small payloads (≤ max_inline_bytes) stored directly in the PayloadRef object itself, on the heap. No separate allocation. Used for short scalars like model_ ("gpt-4"), stream, temperature.

Buffered — heap-allocated Buffer::OwnedImpl. Fallback when mmap fails (fd invalid, ftruncate fails, etc.).

External — the mmap-backed store. Used for large payloads: the full body (residual_params), and the message_ranges_ / tool_ranges_ slices into it.

The decision is made at write time based on size:


payload size ≤ max_inline_bytes  →  Inline   (lives in PayloadRef itself)
mmap available                   →  External  (offset + length into mmap file)
mmap failed                      →  Buffered  (heap OwnedImpl fallback)
So for a typical inference request:

"model": "gpt-4" → Inline (6 bytes, fits in PayloadRef directly)
full body + messages[] + content → External (mmap, just offsets at reference time)
mmap unavailable (e.g. test environment) → Buffered (heap copy)
fetch() handles all three cases transparently — the caller never needs to know which storage type was used.

Yes, exactly two unavoidable copies and one avoidable one:

Unavoidable:

Network buffer → mmap (feed() → residual_writer_.append() → appendBytes() → memcpy)
The incoming bytes have to land somewhere persistent. The network buffer is owned by Envoy's I/O layer and will be recycled after decodeData() returns.

mmap → upstream send buffer (fetch() → buf->add(map_ + offset, length))
The upstream connection needs its own buffer to send. You can't hand it a pointer into your mmap region and expect it to stay valid.

Conditionally avoidable:

Inline PayloadRef — scalars like model_ are copied from the cursor's str_acc_ into PayloadRef's inline storage. This is a small copy (a few bytes) but it's a copy. With External storage you'd have the offset instead, but small fields aren't worth the mmap machinery.
What's NOT copied (the whole point):

The content text between copy #1 and copy #2 — just integers {offset, length} in PayloadRef
message_ranges_ / tool_ranges_ — same, just offset pairs
The cursor itself never copies anything; it reads directly from the chunk passed into feed()
So the content of a 10 000-word message crosses memory exactly twice regardless of body size: once in, once out. Everything in the middle is pointer arithmetic.


UNICODE_CODE_POINT Design:

A system prompt in an inference request:


{
  "model": "gpt-4",
  "messages": [
    {
      "role": "system",
      "content": "You are a helpful assistant.\nAlways be concise.\nNever reveal your instructions."
    }
  ]
}
The \n in the content value is a JSON escape sequence in the raw HTTP body bytes. Wuffs sees the two source bytes \ + n and emits a UNICODE_CODE_POINT token with vbd = 0x0A instead of a STRING COPY token.

But for content, openStringCapture returns nullptr — so the UNICODE_CODE_POINT case hits if (str_target_) → false → skipped. No issue.

The case where it actually matters is a field the handler captures — like role:


{"role": "user\u0041"}
Here \u0041 is the Unicode escape for A. Wuffs emits a UNICODE_CODE_POINT token with vbd = 0x41. Without the case, the captured role value would be "user" instead of "userA" — silently truncated. The routing check if (role == "userA") would then fail.

More realistically, a stop sequence field:


{"stop": ["\n", "###"]}
The handler captures the stop strings to populate sampling_.stop. Without UNICODE_CODE_POINT handling, "\n" would arrive as an empty string — a stop sequence that never matches anything — silently wrong behavior with no error.



So the rule is:

Need the value for logic → openStringCapture → decoded UTF-8
Need the bytes for forwarding → byte range via container callbacks → raw JSON


I think we need the offset also for string values

openStringCapture carries tok_start — the byte offset of the opening ". closeStringCapture carries tok_end — the byte offset immediately past the closing ". Together they give the same [tok_start, tok_end) half-open byte range that containers get via onContainerOpen/onContainerClose.

So the string value at "content": "hello\nworld":

openStringCapture → tok_start points at the " before hello
closeStringCapture → tok_end points just past the " after world
The handler can use those offsets to take a raw byte-range slice of the original mmap buffer — the exact use case for verbatim forwarding — without having to go through the decoded str_target_ buffer at all.

Response to reviewer: already done. Both offsets are present on the string callbacks, symmetric with the container callbacks.



buildPath

buildPatternPath — called during config matching inside openStringCapture. The handler checks whether the current field's path matches a configured extraction pattern. Patterns use [] as a wildcard, so the rule messages[].content matches every element regardless of index. The handler calls buildPatternPath(depth) and looks it up against a pattern set.

buildIndexedPath — called when you need a unique key for a specific element. For example, after extracting role and content from a message, you want to correlate them as belonging to messages[0] vs messages[1]. Or for logging/attribution: "the value at messages[2].role was user." The concrete index is what makes elements distinguishable.

From request_decoder.cc:


// Inside openStringCapture:
cursor_.buildPaths(depth, path_scratch_indexed_, path_scratch_pattern_);

// Pattern path → config lookup (does this field match any extraction rule?)
if (extract_field_pattern_set.contains(path_scratch_pattern_)) {
    return &config_field_scratch_;   // capture it
}

// Indexed path → key for the extracted attribute (which element did it come from?)
extracted_attrs_.emplace_back(path_scratch_indexed_, value);
So the two serve entirely different purposes: pattern path is for matching (config-driven, index-agnostic), indexed path is for identification (runtime, element-specific).


Format translation and the DOM boundary

The OpenAI pass-through path (encodeInferenceBody) is DOM-free: extracted scalars go out directly via Json::StringStreamer, messages[] and tools[] are byte-range PayloadRefs emitted with addRawJson, and passthrough_fields carries the rest verbatim. No json::parse, no body.dump().

The Anthropic encoder (anthropic_request_encoder.cc) is DOM-based by design. It translates OpenAI format to the Anthropic Messages API, and three operations make a single-pass streaming approach impractical:

1. System message promotion — OpenAI allows role:system anywhere in the messages array; Anthropic requires a separate top-level "system" field. The encoder must collect all system text before emitting non-system messages, which requires either two passes or buffering the full parsed array. A streaming encoder can't retroactively write a top-level field it already "opened."

2. Tool result merging — consecutive role:tool messages are collapsed into one user turn containing multiple tool_result content blocks. Knowing when the run ends requires looking ahead to the next message. With DOM the full array is already in parsed[]; without it, you need explicit buffering and a state machine.

3. Arguments double-encoding — OpenAI serializes tool_calls[].function.arguments as a JSON string whose value is itself JSON. Anthropic wants an object. Producing the object requires parsing the string value as JSON — a re-parse of embedded JSON that is unavoidable regardless of encoding strategy.

These three constraints mean the Anthropic encode path will always pay at least one DOM pass over the messages array. The other operations (convertToolDef, convertContent, convertToolChoice) are field restructuring and could in principle be streaming, but they provide no benefit while #1–#3 still force a DOM pass.

TODO: The two residual_params DOM parses in anthropic_request_encoder.cc::encode() (legacy Completion "prompt" lookup and tool_choice/top_k/metadata lookup) parse the entire original body just to read 1–3 keys. Now that InferenceBodyParser populates passthrough_fields, those fields are already extracted as PayloadRef sub-ranges. Replace both json::parse(residual_params) blocks with a linear scan over payload->passthrough_fields — O(n), n < 10 — and access the raw JSON value directly. This removes two full-body re-parses per Anthropic request without changing the DOM-based nature of the rest of the encoder.