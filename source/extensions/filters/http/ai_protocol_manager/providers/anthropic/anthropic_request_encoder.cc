#include "source/extensions/filters/http/ai_protocol_manager/providers/anthropic/anthropic_request_encoder.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

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

// Collects consecutive OpenAI `role: "tool"` messages starting at `idx` into
// a single Anthropic user message containing an array of tool_result blocks.
// Advances `idx` past all consumed tool messages.
//
// OpenAI:   {"role":"tool","tool_call_id":"...","content":"..."}  (one per result)
// Anthropic: {"role":"user","content":[{"type":"tool_result","tool_use_id":"...","content":"..."},...]}
json collectToolResults(const std::vector<json>& parsed, size_t& idx) {
  json blocks = json::array();
  while (idx < parsed.size()) {
    const json& msg = parsed[idx];
    if (msg.value("role", "") != "tool") {
      break;
    }
    json tr = {{"type", "tool_result"}};
    if (msg.contains("tool_call_id")) {
      tr["tool_use_id"] = msg["tool_call_id"];
    }
    if (msg.contains("content")) {
      // Content may be a string or an array of content blocks.
      tr["content"] = msg["content"];
    }
    blocks.push_back(std::move(tr));
    ++idx;
  }
  return {{"role", "user"}, {"content", std::move(blocks)}};
}

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

// Builds the Anthropic messages array and extracts the system prompt(s) from
// an OpenAI messages PayloadRef vector.
//
// Rules applied:
//   - role=system  → concatenated into `system_out` (not added to messages).
//   - role=tool    → consecutive runs merged into one user turn (tool_result).
//   - role=assistant + tool_calls → tool_use content blocks.
//   - role=user/assistant (plain) → content blocks converted.
void buildMessages(const std::vector<Codec::PayloadRef>& refs, json& messages_out,
                   json& system_out, const Codec::AiRequest& request) {
  // Pre-parse every ref so we can do lookahead for tool-result grouping.
  std::vector<json> parsed;
  parsed.reserve(refs.size());
  for (const auto& ref : refs) {
    if (ref.empty()) {
      continue;
    }
    auto msg = json::parse(Codec::convertPayloadRefToString(ref, request), nullptr, /*allow_exceptions=*/false);
    if (!msg.is_discarded()) {
      parsed.push_back(std::move(msg));
    }
  }

  // Collect system messages first (may appear anywhere in the array per OpenAI spec).
  std::string system_text;
  for (const auto& msg : parsed) {
    if (msg.value("role", "") == "system") {
      const auto& c = msg["content"];
      if (!system_text.empty()) {
        system_text += "\n\n";
      }
      if (c.is_string()) {
        system_text += c.get<std::string>();
      } else if (c.is_array()) {
        // Flatten text blocks from system content array.
        for (const auto& block : c) {
          if (block.value("type", "") == "text") {
            system_text += block.value("text", "");
          }
        }
      }
    }
  }
  if (!system_text.empty()) {
    system_out = std::move(system_text);
  }

  // Convert the non-system messages in order.
  messages_out = json::array();
  size_t i = 0;
  while (i < parsed.size()) {
    const json& msg = parsed[i];
    const std::string role = msg.value("role", "");

    if (role == "system") {
      ++i;
      continue;
    }

    if (role == "tool") {
      // Merge consecutive tool messages into one user turn.
      messages_out.push_back(collectToolResults(parsed, i));
      continue; // i already advanced by collectToolResults
    }

    if (role == "assistant" && msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
        !msg["tool_calls"].empty()) {
      messages_out.push_back(convertAssistantWithToolCalls(msg));
      ++i;
      continue;
    }

    // Plain user or assistant message.
    json anthropic_msg = {{"role", role}};
    if (msg.contains("content")) {
      anthropic_msg["content"] = convertContent(msg["content"]);
    }
    messages_out.push_back(std::move(anthropic_msg));
    ++i;
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
  json system_val = json(nullptr);

  if (is_chat) {
    buildMessages(payload->messages, messages, system_val, request);
  } else {
    // Legacy Completion: pull `prompt` from residual_params and wrap it.
    std::string prompt_text;
    if (!payload->residual_params.empty()) {
      auto residual =
          json::parse(Codec::convertPayloadRefToString(payload->residual_params, request), nullptr, /*allow_exceptions=*/false);
      if (!residual.is_discarded() && residual.contains("prompt")) {
        const auto& p = residual["prompt"];
        if (p.is_string()) {
          prompt_text = p.get<std::string>();
        }
      }
    }
    if (!prompt_text.empty()) {
      messages.push_back({{"role", "user"}, {"content", prompt_text}});
    }
  }

  if (!system_val.is_null()) {
    body["system"] = std::move(system_val);
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

  // ── tool_choice (from residual_params) ────────────────────────────────────
  // tool_choice is not extracted into a structured field by InferenceBodyParser,
  // so we read it back from the full original body stored in residual_params.
  if (!payload->residual_params.empty()) {
    auto residual =
        json::parse(Codec::convertPayloadRefToString(payload->residual_params, request), nullptr, /*allow_exceptions=*/false);
    if (!residual.is_discarded() && residual.contains("tool_choice")) {
      body["tool_choice"] = convertToolChoice(residual["tool_choice"]);
    }
    // Pass through Anthropic-specific extras that OpenAI callers may inject
    // into residual_params (top_k, metadata, etc.).
    for (const auto& key : {"top_k", "metadata"}) {
      if (residual.contains(key)) {
        body[key] = residual[key];
      }
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
