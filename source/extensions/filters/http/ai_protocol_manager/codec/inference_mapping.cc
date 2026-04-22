#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_mapping.h"

#include <string>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/json/json_loader.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

// Re-serialize a nested JSON object back to a canonical string. We parse the
// OpenAI body once into Json::Object and later need to ship opaque sub-trees
// (function.parameters, response_format.json_schema.schema, guided_json) into
// downstream encoders that either take raw JSON or want to re-parse into
// their own proto. asJsonString() on Json::Object is the cleanest round-trip.
std::string reserializeObject(const Json::Object& obj) { return obj.asJsonString(); }

// Drain the body into residual_params for OPENAI_PASSTHROUGH round-trip. Kept
// as a helper so both the fast-path parser and error-recovery paths share it.
absl::Status stashRawBody(absl::string_view body, AiRequest& req) {
  if (req.payload_store == nullptr) {
    return absl::FailedPreconditionError("AiRequest has no payload store");
  }
  auto buf = std::make_unique<Buffer::OwnedImpl>();
  buf->add(body);
  auto ref = req.payload_store->store(std::move(buf), PayloadKind::Other);
  auto* payload = req.asInference();
  if (payload == nullptr) {
    payload = &req.payload.emplace<InferencePayload>();
  }
  payload->residual_params = std::move(ref);
  return absl::OkStatus();
}

// Parse a single OpenAI "messages[i]" entry. Phase 3a: text content only.
// Returns a ChatMessage with role + flattened text. Non-text content parts
// (image_url, input_audio, ...) are dropped with a note in the text field;
// proper handling lands in Phase 3c.
ChatMessage parseMessageEntry(const Json::Object& msg) {
  ChatMessage out;
  out.role = msg.getString("role", "").value_or("");

  // content can be a string OR an array of content parts. We only inspect it
  // via the getString path first; if that fails we fall back to array form.
  auto content_str_or = msg.getString("content");
  if (content_str_or.ok()) {
    out.text = *content_str_or;
    return out;
  }

  // Array form: walk parts looking for type=="text".text.
  auto parts_or = msg.getObjectArray("content", /*allow_empty=*/true);
  if (!parts_or.ok()) {
    return out;
  }
  std::string combined;
  for (const auto& part : *parts_or) {
    const std::string type = part->getString("type", "").value_or("");
    if (type == "text") {
      const auto text_or = part->getString("text");
      if (text_or.ok()) {
        if (!combined.empty()) {
          combined += "\n";
        }
        combined += *text_or;
      }
    }
    // Phase 3c: image_url, input_audio. Today these fall through silently.
  }
  out.text = std::move(combined);
  return out;
}

// OPENAI_VERTEX_SPEC.md §2.2 — walk tools[]. image_generation is rejected;
// function / google_search / enterprise_search become typed Tool entries.
absl::Status parseTools(const Json::Object& root, InferencePayload& payload) {
  auto tools_or = root.getObjectArray("tools", /*allow_empty=*/true);
  if (!tools_or.ok()) {
    return absl::OkStatus();  // No tools[] in request; not an error.
  }
  for (const auto& tool_obj : *tools_or) {
    const std::string type = tool_obj->getString("type", "").value_or("");
    if (type == "function") {
      Tool tool;
      tool.kind = ToolKind::Function;
      auto fn_or = tool_obj->getObject("function", /*allow_empty=*/true);
      if (fn_or.ok() && *fn_or != nullptr) {
        const auto& fn = *fn_or;
        tool.function.name = fn->getString("name", "").value_or("");
        tool.function.description = fn->getString("description", "").value_or("");
        if (fn->hasObject("parameters")) {
          auto params_or = fn->getObject("parameters", /*allow_empty=*/true);
          if (params_or.ok() && *params_or != nullptr) {
            tool.function.parameters_json = reserializeObject(**params_or);
          }
        }
      }
      if (tool.function.name.empty()) {
        return absl::InvalidArgumentError("tools[].function.name is required");
      }
      payload.tools.push_back(std::move(tool));
    } else if (type == "google_search") {
      Tool tool;
      tool.kind = ToolKind::GoogleSearch;
      payload.tools.push_back(std::move(tool));
    } else if (type == "enterprise_search") {
      Tool tool;
      tool.kind = ToolKind::EnterpriseWebSearch;
      payload.tools.push_back(std::move(tool));
    } else if (type == "image_generation") {
      return absl::InvalidArgumentError("tool-type image_generation not supported");
    } else {
      return absl::InvalidArgumentError(absl::StrCat("unsupported tool type: ", type));
    }
  }
  return absl::OkStatus();
}

// OPENAI_VERTEX_SPEC.md §2.3 — tool_choice can be a string or an object.
absl::Status parseToolChoice(const Json::Object& root, InferencePayload& payload) {
  // String form first.
  if (auto s = root.getString("tool_choice"); s.ok()) {
    if (*s == "auto") {
      payload.tool_choice.mode = ToolChoice::Mode::Auto;
    } else if (*s == "none") {
      payload.tool_choice.mode = ToolChoice::Mode::None;
    } else if (*s == "required") {
      payload.tool_choice.mode = ToolChoice::Mode::Required;
    } else {
      return absl::InvalidArgumentError(absl::StrCat("unsupported tool_choice value: ", *s));
    }
    return absl::OkStatus();
  }
  // Object form: {type: "function", function: {name: "foo"}}.
  if (!root.hasObject("tool_choice")) {
    return absl::OkStatus();
  }
  auto tc_or = root.getObject("tool_choice", /*allow_empty=*/true);
  if (!tc_or.ok() || *tc_or == nullptr) {
    return absl::OkStatus();
  }
  const auto& tc = *tc_or;
  const std::string type = tc->getString("type", "").value_or("");
  if (type != "function") {
    return absl::InvalidArgumentError("tool_choice type must be 'function' when object-form");
  }
  auto fn_or = tc->getObject("function", /*allow_empty=*/true);
  if (!fn_or.ok() || *fn_or == nullptr) {
    return absl::InvalidArgumentError("tool_choice.function is required when type=function");
  }
  payload.tool_choice.mode = ToolChoice::Mode::NamedFunction;
  payload.tool_choice.function_name = (*fn_or)->getString("name", "").value_or("");
  if (payload.tool_choice.function_name.empty()) {
    return absl::InvalidArgumentError("tool_choice.function.name is required");
  }
  return absl::OkStatus();
}

// OPENAI_VERTEX_SPEC.md §2.4 + guided_* rows. Enforces the rule
// "at most one of response_format / guided_choice / guided_regex / guided_json".
absl::Status parseResponseFormat(const Json::Object& root, InferencePayload& payload) {
  int specified = 0;

  if (root.hasObject("response_format")) {
    ++specified;
    auto rf_or = root.getObject("response_format", /*allow_empty=*/true);
    if (rf_or.ok() && *rf_or != nullptr) {
      const auto& rf = *rf_or;
      const std::string type = rf->getString("type", "").value_or("");
      if (type == "text") {
        payload.response_format.kind = ResponseFormat::Kind::Text;
      } else if (type == "json_object") {
        payload.response_format.kind = ResponseFormat::Kind::JsonObject;
      } else if (type == "json_schema") {
        payload.response_format.kind = ResponseFormat::Kind::JsonSchema;
        auto js_or = rf->getObject("json_schema", /*allow_empty=*/true);
        if (js_or.ok() && *js_or != nullptr) {
          auto schema_or = (*js_or)->getObject("schema", /*allow_empty=*/true);
          if (schema_or.ok() && *schema_or != nullptr) {
            payload.response_format.schema_json = reserializeObject(**schema_or);
          }
        }
        if (payload.response_format.schema_json.empty()) {
          return absl::InvalidArgumentError("response_format.json_schema.schema is required");
        }
      } else if (!type.empty()) {
        return absl::InvalidArgumentError(absl::StrCat("unsupported response_format.type: ", type));
      }
    }
  }

  if (auto arr = root.getStringArray("guided_choice", /*allow_empty=*/true); arr.ok()) {
    if (!arr->empty()) {
      ++specified;
      payload.response_format.kind = ResponseFormat::Kind::GuidedChoice;
      payload.response_format.choices.assign(arr->begin(), arr->end());
    }
  }

  if (auto s = root.getString("guided_regex"); s.ok() && !s->empty()) {
    ++specified;
    payload.response_format.kind = ResponseFormat::Kind::GuidedRegex;
    payload.response_format.regex = *s;
  }

  if (root.hasObject("guided_json")) {
    auto gj_or = root.getObject("guided_json", /*allow_empty=*/true);
    if (gj_or.ok() && *gj_or != nullptr) {
      ++specified;
      payload.response_format.kind = ResponseFormat::Kind::GuidedJson;
      payload.response_format.schema_json = reserializeObject(**gj_or);
    }
  }

  if (specified > 1) {
    return absl::InvalidArgumentError(
        "only one of response_format, guided_choice, guided_regex, guided_json can be specified");
  }
  return absl::OkStatus();
}

} // namespace

absl::Status parseOpenAIInferenceRequest(absl::string_view body, InferenceInvocation invocation,
                                         AiRequest& req) {
  if (req.payload_store == nullptr) {
    return absl::FailedPreconditionError("AiRequest has no payload store");
  }

  // Initialize payload + stash raw body for the OPENAI_PASSTHROUGH round-trip
  // fallback.
  InferencePayload& payload = req.payload.emplace<InferencePayload>();
  payload.invocation = invocation;
  req.protocol = ProtocolKind::Inference;

  auto raw_st = stashRawBody(body, req);
  if (!raw_st.ok()) {
    return raw_st;
  }

  if (body.empty()) {
    return absl::OkStatus();
  }

  auto root_or = Json::Factory::loadFromString(std::string(body));
  if (!root_or.ok()) {
    // Leave payload parsed-empty; residual_params has the raw body for
    // pass-through. Gemini encoder will error out (it requires parsed fields).
    return root_or.status();
  }
  const auto& root = *root_or;

  payload.target.name = root->getString("model", "").value_or("");
  payload.streaming = root->getBoolean("stream", false).value_or(false);

  // Sampling — see OPENAI_VERTEX_SPEC.md §2.
  if (auto t = root->getDouble("temperature"); t.ok()) {
    payload.sampling.temperature = *t;
  }
  if (auto t = root->getDouble("top_p"); t.ok()) {
    payload.sampling.top_p = *t;
  }
  // max_completion_tokens wins over max_tokens when both present.
  if (auto mt = root->getInteger("max_completion_tokens"); mt.ok()) {
    payload.sampling.max_tokens = static_cast<int32_t>(*mt);
  } else if (auto mt2 = root->getInteger("max_tokens"); mt2.ok()) {
    payload.sampling.max_tokens = static_cast<int32_t>(*mt2);
  }
  if (auto n = root->getInteger("n"); n.ok()) {
    payload.sampling.n = static_cast<int32_t>(*n);
  }
  if (auto s = root->getInteger("seed"); s.ok()) {
    payload.sampling.seed = *s;
  }
  if (auto p = root->getDouble("frequency_penalty"); p.ok()) {
    payload.sampling.frequency_penalty = *p;
  }
  if (auto p = root->getDouble("presence_penalty"); p.ok()) {
    payload.sampling.presence_penalty = *p;
  }
  // stop: string or array-of-string.
  if (auto s = root->getString("stop"); s.ok()) {
    payload.sampling.stop.push_back(*s);
  } else if (auto arr = root->getStringArray("stop", /*allow_empty=*/true); arr.ok()) {
    payload.sampling.stop.assign(arr->begin(), arr->end());
  }

  // Messages. OPENAI_VERTEX_SPEC.md §2.1: system / developer roles merge into
  // systemInstruction; user / assistant / tool turns populate contents.
  auto messages_or = root->getObjectArray("messages", /*allow_empty=*/true);
  if (messages_or.ok()) {
    for (const auto& msg : *messages_or) {
      ChatMessage m = parseMessageEntry(*msg);
      if (m.role == "system" || m.role == "developer") {
        if (!m.text.empty()) {
          payload.system_instructions.push_back(std::move(m.text));
        }
      } else if (!m.role.empty()) {
        payload.chat.push_back(std::move(m));
      }
    }
  }

  // Phase 3b — tools, tool_choice, response_format / guided_*. Each is
  // independent of the chat content but fails the whole parse on a semantic
  // violation (bad tool type, multiple response-format directives, ...).
  if (auto st = parseTools(*root, payload); !st.ok()) {
    return st;
  }
  if (auto st = parseToolChoice(*root, payload); !st.ok()) {
    return st;
  }
  if (auto st = parseResponseFormat(*root, payload); !st.ok()) {
    return st;
  }

  return absl::OkStatus();
}

absl::Status OpenAiEncoder::encode(const AiRequest& req, Buffer::Instance& out) {
  const InferencePayload* payload = req.asInference();
  if (payload == nullptr) {
    return absl::InvalidArgumentError("OpenAiEncoder expects an Inference payload");
  }
  // Phase 3a: round-trip via residual_params (the raw OpenAI body the parser
  // stashed). Phase 3b+ will synthesize OpenAI-shape JSON from the parsed
  // fields once the parser covers tools / multimodal / etc.
  const PayloadRef& ref = payload->residual_params;
  switch (ref.storage()) {
  case PayloadRef::Storage::Inline:
    out.add(ref.inlineView());
    return absl::OkStatus();
  case PayloadRef::Storage::Buffered:
    out.add(ref.buffered());
    return absl::OkStatus();
  case PayloadRef::Storage::External:
    return absl::UnimplementedError("External payload resolution not yet implemented");
  }
  return absl::InternalError("unknown PayloadRef storage");
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
