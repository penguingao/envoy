#include "source/extensions/filters/http/ai_protocol_manager/providers/anthropic/anthropic_request_encoder.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/wuffs_json.h"

#include "absl/strings/match.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Providers {

namespace {

using nlohmann::json;

// ── Content block converters ──────────────────────────────────────────────────

// Converts an OpenAI image_url content block to the Anthropic image block.
//
// OpenAI:   {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<b64>"}}
// Anthropic: {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":"<b64>"}}
//
// OpenAI:   {"type":"image_url","image_url":{"url":"https://..."}}
// Anthropic: {"type":"image","source":{"type":"url","url":"https://..."}}
json convertImageBlock(const json& block) {
  json result = {{"type", "image"}};
  if (!block.contains("image_url")) {
    return result;
  }
  const std::string url = block["image_url"].value("url", "");
  if (absl::StartsWith(url, "data:")) {
    // data:<media_type>;base64,<data>
    const auto semi = url.find(';');
    const auto comma = url.find(',');
    if (semi != std::string::npos && comma != std::string::npos) {
      result["source"] = {
          {"type", "base64"},
          {"media_type", url.substr(5, semi - 5)},
          {"data", url.substr(comma + 1)},
      };
    }
  } else {
    result["source"] = {{"type", "url"}, {"url", url}};
  }
  return result;
}

// Converts a single OpenAI user/assistant content block to the Anthropic
// equivalent. Unknown block types are passed through unchanged so forward
// compatibility is preserved.
json convertContentBlock(const json& block) {
  const std::string type = block.value("type", "");
  if (type == "text") {
    return {{"type", "text"}, {"text", block.value("text", "")}};
  }
  if (type == "image_url") {
    return convertImageBlock(block);
  }
  return block; // pass through tool_use, tool_result, document, etc.
}

// Converts an OpenAI content value (string or array of blocks) to the
// Anthropic representation. String content is kept as-is since Anthropic
// also accepts plain strings; arrays are block-converted.
json convertContent(const json& content) {
  if (content.is_string() || content.is_null()) {
    return content;
  }
  if (content.is_array()) {
    json result = json::array();
    for (const auto& block : content) {
      result.push_back(convertContentBlock(block));
    }
    return result;
  }
  return json::array();
}

// ── Tool definition converter ─────────────────────────────────────────────────

// Converts an OpenAI tool definition to the Anthropic tool definition.
//
// OpenAI:   {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}
// Anthropic: {"name":"...","description":"...","input_schema":{...}}
json convertToolDef(const json& openai_tool) {
  json result = json::object();
  if (!openai_tool.contains("function")) {
    return openai_tool; // already Anthropic-format; pass through
  }
  const auto& fn = openai_tool["function"];
  result["name"] = fn.value("name", "");
  if (fn.contains("description")) {
    result["description"] = fn["description"];
  }
  // "parameters" in OpenAI is a JSON Schema object — Anthropic calls it
  // "input_schema". They share the same JSON Schema dialect, so no rewriting
  // is required beyond the key rename.
  if (fn.contains("parameters")) {
    result["input_schema"] = fn["parameters"];
  } else {
    result["input_schema"] = {{"type", "object"}, {"properties", json::object()}};
  }
  return result;
}

// ── Tool choice converter ─────────────────────────────────────────────────────

// Converts an OpenAI tool_choice value to the Anthropic object form.
//
// OpenAI string shortcuts:
//   "none"     → {"type":"none"}
//   "auto"     → {"type":"auto"}
//   "required" → {"type":"any"}    (Anthropic forces at least one tool call)
//
// OpenAI named function:
//   {"type":"function","function":{"name":"..."}} → {"type":"tool","name":"..."}
json convertToolChoice(const json& tc) {
  if (tc.is_string()) {
    const std::string s = tc.get<std::string>();
    if (s == "none") {
      return {{"type", "none"}};
    }
    if (s == "required") {
      return {{"type", "any"}};
    }
    return {{"type", "auto"}};
  }
  if (tc.is_object()) {
    if (tc.value("type", "") == "function" && tc.contains("function")) {
      return {{"type", "tool"}, {"name", tc["function"].value("name", "")}};
    }
    return tc; // already Anthropic-format; pass through
  }
  return {{"type", "auto"}};
}

// ── Message processing ────────────────────────────────────────────────────────

// Converts an OpenAI assistant message that carries tool_calls into the
// Anthropic representation where tool_calls become tool_use content blocks.
//
// OpenAI:
//   {"role":"assistant","content":null,
//    "tool_calls":[{"id":"...","type":"function","function":{"name":"...","arguments":"{...}"}}]}
//
// Anthropic:
//   {"role":"assistant","content":[
//     {"type":"tool_use","id":"...","name":"...","input":{...}}]}
json convertAssistantWithToolCalls(const json& msg) {
  json content = json::array();

  // Preserve any text portion of the assistant turn.
  if (msg.contains("content") && !msg["content"].is_null()) {
    const auto& c = msg["content"];
    if (c.is_string() && !c.get<std::string>().empty()) {
      content.push_back({{"type", "text"}, {"text", c.get<std::string>()}});
    } else if (c.is_array()) {
      for (const auto& block : c) {
        if (block.value("type", "") == "text") {
          content.push_back(block);
        }
      }
    }
  }

  for (const auto& tc : msg["tool_calls"]) {
    json tu = {{"type", "tool_use"}};
    if (tc.contains("id")) {
      tu["id"] = tc["id"];
    }
    if (tc.contains("function")) {
      const auto& fn = tc["function"];
      tu["name"] = fn.value("name", "");
      if (fn.contains("arguments")) {
        // OpenAI serializes arguments as a JSON string; Anthropic wants an object.
        const auto args_str = fn["arguments"].get<std::string>();
        auto args = json::parse(args_str, nullptr, /*allow_exceptions=*/false);
        tu["input"] = args.is_discarded() ? json::object() : std::move(args);
      } else {
        tu["input"] = json::object();
      }
    }
    content.push_back(std::move(tu));
  }

  return {{"role", "assistant"}, {"content", std::move(content)}};
}

// Emits an Anthropic user turn that merges a pre-grouped set of consecutive
// OpenAI `role: "tool"` message refs into a single tool_result block array.
// Each ref is parsed individually — O(1) peak heap per message.
//
// OpenAI:   {"role":"tool","tool_call_id":"...","content":"..."}  (one per result)
// Anthropic: {"role":"user","content":[{"type":"tool_result","tool_use_id":"...","content":"..."},...]}
json collectToolResults(const std::vector<Codec::PayloadRef>& refs,
                        const Codec::AiRequest& request) {
  json blocks = json::array();
  for (const auto& ref : refs) {
    auto msg = json::parse(Codec::convertPayloadRefToString(ref, request),
                           nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) continue;
    json tr = {{"type", "tool_result"}};
    if (msg.contains("tool_call_id")) tr["tool_use_id"] = msg["tool_call_id"];
    if (msg.contains("content"))     tr["content"]      = msg["content"];
    blocks.push_back(std::move(tr));
  }
  return {{"role", "user"}, {"content", std::move(blocks)}};
}

// Organized form of messages[] produced in one pass for Anthropic encoding.
// Kept local to this file — InferencePayload has no Anthropic-specific fields.
struct PreparedMessages {
  std::string system_content;
  struct Entry {
    std::string             role;       // empty = merged tool group
    Codec::PayloadRef       ref;        // single message (when role non-empty)
    std::vector<Codec::PayloadRef> tool_refs;  // tool group (when role empty)
  };
  std::vector<Entry> entries;
};

// One-pass message preparation. Uses WuffsJsonObjectReader to extract only the
// `role` field from each message (no full DOM). Routes:
//   role=system  → content extracted into result.system_content
//   role=tool    → buffered into a pending group; flushed as a single entry on break
//   other roles  → flush pending group then store as a tagged entry
//
// Replaces the old two-pass approach (full pre-parse of all messages into
// std::vector<json> + separate system scan). Peak heap is now O(1) per message
// for role extraction; full parse only for messages that need conversion.
PreparedMessages prepareAnthropicMessages(const Codec::InferencePayload& payload,
                                          const Codec::AiRequest& request) {
  PreparedMessages result;
  std::vector<Codec::PayloadRef> pending_tools;

  auto flush_tools = [&]() {
    if (!pending_tools.empty()) {
      result.entries.push_back({"", {}, std::move(pending_tools)});
      pending_tools.clear();
    }
  };

  for (const auto& ref : payload.messages) {
    if (ref.empty()) continue;

    // Lightweight role extraction — only the role string is decoded here.
    std::string role;
    {
      struct RoleExtractor : Codec::WuffsJsonObjectReader::Handler {
        std::string& out;
        explicit RoleExtractor(std::string& r) : out(r) {}
        void onStringField(absl::string_view key, std::string value) override {
          if (key == "role") out = std::move(value);
        }
      } h(role);
      (void)Codec::WuffsJsonObjectReader::read(
          Codec::convertPayloadRefToString(ref, request), h);
    }

    if (role == "system") {
      flush_tools();
      auto msg = json::parse(Codec::convertPayloadRefToString(ref, request),
                             nullptr, /*allow_exceptions=*/false);
      if (!msg.is_discarded() && msg.contains("content")) {
        const auto& c = msg["content"];
        if (!result.system_content.empty())
          result.system_content += "\n\n";
        if (c.is_string()) {
          result.system_content += c.get<std::string>();
        } else if (c.is_array()) {
          for (const auto& block : c) {
            if (block.value("type", "") == "text")
              result.system_content += block.value("text", "");
          }
        }
      }
      continue;
    }

    if (role == "tool") {
      pending_tools.push_back(ref.clone());
      continue;
    }

    flush_tools();
    result.entries.push_back({std::move(role), ref.clone(), {}});
  }

  flush_tools();
  return result;
}

// Serializes a PreparedMessages into the Anthropic JSON messages array.
// Each entry is parsed individually — no simultaneous full DOM.
void buildMessages(const PreparedMessages& prepared, json& messages_out,
                   const Codec::AiRequest& request) {
  messages_out = json::array();
  for (const auto& entry : prepared.entries) {
    if (entry.role.empty()) {
      messages_out.push_back(collectToolResults(entry.tool_refs, request));
      continue;
    }

    auto msg = json::parse(Codec::convertPayloadRefToString(entry.ref, request),
                           nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) continue;

    if (entry.role == "assistant" &&
        msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
        !msg["tool_calls"].empty()) {
      messages_out.push_back(convertAssistantWithToolCalls(msg));
      continue;
    }

    json anthropic_msg = {{"role", entry.role}};
    if (msg.contains("content"))
      anthropic_msg["content"] = convertContent(msg["content"]);
    messages_out.push_back(std::move(anthropic_msg));
  }
}

} // namespace

// ── AnthropicRequestEncoder::encode ──────────────────────────────────────────

absl::optional<Codec::RestHttpRequest>
AnthropicRequestEncoder::encode(const Codec::AiRequest& request) {
  const Codec::InferencePayload* payload = request.as_inference();
  if (payload == nullptr) {
    return absl::nullopt;
  }

  // Only ChatCompletion and legacy Completion have a clear Anthropic mapping.
  const bool is_chat = payload->invocation == Codec::InferenceInvocation::ChatCompletion;
  const bool is_legacy = payload->invocation == Codec::InferenceInvocation::Completion;
  if (!is_chat && !is_legacy) {
    return absl::nullopt;
  }

  json body = json::object();

  // ── model ─────────────────────────────────────────────────────────────────
  if (!payload->target.name.empty()) {
    body["model"] = payload->target.name;
  }

  // ── max_tokens (required by Anthropic) ────────────────────────────────────
  // Default to 4096 when the OpenAI request omitted max_tokens.
  body["max_tokens"] = payload->sampling.max_tokens.value_or(4096);

  // ── stream ────────────────────────────────────────────────────────────────
  body["stream"] = request.streaming;

  // ── sampling params ───────────────────────────────────────────────────────
  if (payload->sampling.temperature.has_value()) {
    body["temperature"] = *payload->sampling.temperature;
  }
  if (payload->sampling.top_p.has_value()) {
    body["top_p"] = *payload->sampling.top_p;
  }
  if (!payload->sampling.stop.empty()) {
    body["stop_sequences"] = payload->sampling.stop;
  }
  // n and seed are not supported by the Anthropic Messages API; dropped.

  // ── messages / system ─────────────────────────────────────────────────────
  json messages = json::array();

  if (is_chat) {
    // One-pass role extraction via WuffsJsonObjectReader; per-message parse
    // only for messages that need format conversion. No full pre-parse.
    const PreparedMessages prepared = prepareAnthropicMessages(*payload, request);
    if (!prepared.system_content.empty()) {
      body["system"] = prepared.system_content;
    }
    buildMessages(prepared, messages, request);
  } else {
    // Legacy Completion: find "prompt" in passthrough_fields (zero-copy sub-ref
    // from the decode pass) — no full-body json::parse needed.
    for (const auto& [key, ref] : payload->passthrough_fields) {
      if (key == "prompt") {
        auto j = json::parse(Codec::convertPayloadRefToString(ref, request),
                             nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.is_string()) {
          messages.push_back({{"role", "user"}, {"content", j.get<std::string>()}});
        }
        break;
      }
    }
  }

  body["messages"] = std::move(messages);

  // ── tools ─────────────────────────────────────────────────────────────────
  if (!payload->tools.empty()) {
    json tools_arr = json::array();
    for (const auto& ref : payload->tools) {
      if (ref.empty()) {
        continue;
      }
      auto tool = json::parse(Codec::convertPayloadRefToString(ref, request), nullptr, /*allow_exceptions=*/false);
      if (!tool.is_discarded()) {
        tools_arr.push_back(convertToolDef(tool));
      }
    }
    if (!tools_arr.empty()) {
      body["tools"] = std::move(tools_arr);
    }
  }

  // ── tool_choice, top_k, metadata (from passthrough_fields) ───────────────
  // These fields land in passthrough_fields as zero-copy sub-refs from the
  // decode pass. Scanning passthrough_fields (n < 10) replaces the previous
  // two full-body json::parse(residual_params) calls.
  for (const auto& [key, ref] : payload->passthrough_fields) {
    if (key == "tool_choice") {
      auto j = json::parse(Codec::convertPayloadRefToString(ref, request),
                           nullptr, /*allow_exceptions=*/false);
      if (!j.is_discarded()) body["tool_choice"] = convertToolChoice(j);
    } else if (key == "top_k" || key == "metadata") {
      auto j = json::parse(Codec::convertPayloadRefToString(ref, request),
                           nullptr, /*allow_exceptions=*/false);
      if (!j.is_discarded()) body[key] = std::move(j);
    }
  }

  return Codec::RestHttpRequest{
      .method = "POST",
      .path = "/v1/messages",
      .body = body.dump(),
  };
}

} // namespace Providers
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
