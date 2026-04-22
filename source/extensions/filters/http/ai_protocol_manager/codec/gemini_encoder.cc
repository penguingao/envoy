#include "source/extensions/filters/http/ai_protocol_manager/codec/gemini_encoder.h"

#include <string>

#include "source/common/protobuf/utility.h"

#include "absl/strings/str_cat.h"
#include "google/protobuf/struct.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

using google::protobuf::ListValue;
using google::protobuf::Struct;
using google::protobuf::Value;

Value stringValue(absl::string_view s) {
  Value v;
  v.set_string_value(std::string(s));
  return v;
}

Value numberValue(double d) {
  Value v;
  v.set_number_value(d);
  return v;
}

Value boolValue(bool b) {
  Value v;
  v.set_bool_value(b);
  return v;
}

Value structValue(Struct s) {
  Value v;
  *v.mutable_struct_value() = std::move(s);
  return v;
}

Value listValue(ListValue l) {
  Value v;
  *v.mutable_list_value() = std::move(l);
  return v;
}

// Build a { role, parts: [{text}] } Gemini Content.
Struct buildContent(absl::string_view role, absl::string_view text) {
  Struct content;
  (*content.mutable_fields())["role"] = stringValue(role);

  ListValue parts_list;
  Struct part;
  (*part.mutable_fields())["text"] = stringValue(text);
  *parts_list.add_values() = structValue(std::move(part));

  (*content.mutable_fields())["parts"] = listValue(std::move(parts_list));
  return content;
}

// OPENAI_VERTEX_SPEC.md §2.1 — OpenAI role → Gemini role for contents[].
// system / developer are handled separately (systemInstruction).
absl::string_view geminiRoleFor(absl::string_view openai_role) {
  if (openai_role == "assistant") {
    return "model";
  }
  // user, tool, anything else → user. Tool responses currently fold into
  // user turns; proper functionResponse parts land in Phase 3c alongside
  // multimodal content.
  return "user";
}

// Parse a JSON-Schema string into a protobuf Struct for embedding into the
// Gemini request body. Returns InvalidArgument when the schema is not a JSON
// object. Schemas arrive here already re-serialized by the parser (see
// reserializeObject in inference_mapping.cc), so this is the only place the
// opaque string becomes structured.
absl::StatusOr<Struct> parseJsonSchemaStruct(absl::string_view json) {
  Struct s;
  auto st = MessageUtil::loadFromJsonNoThrow(json, s);
  if (!st.ok()) {
    return st;
  }
  return s;
}

// §2.2 — emit tools[]. We bundle every FunctionDeclaration into ONE Gemini
// Tool entry (several Gemini model versions reject multiple Tool wrappers).
// GoogleSearch / EnterpriseWebSearch each get their own entry.
absl::Status encodeTools(const std::vector<Tool>& tools, Struct& root_fields_parent) {
  if (tools.empty()) {
    return absl::OkStatus();
  }
  ListValue tools_list;
  ListValue function_decls;
  for (const auto& t : tools) {
    switch (t.kind) {
    case ToolKind::Function: {
      Struct decl;
      auto& df = *decl.mutable_fields();
      df["name"] = stringValue(t.function.name);
      if (!t.function.description.empty()) {
        df["description"] = stringValue(t.function.description);
      }
      if (!t.function.parameters_json.empty()) {
        auto schema_or = parseJsonSchemaStruct(t.function.parameters_json);
        if (!schema_or.ok()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "tool ", t.function.name, " parameters are not valid JSON schema"));
        }
        // Gemini 2.5+ accepts parametersJsonSchema verbatim. Older models want
        // a converted `parameters` Schema; Phase 3e gates this by model name.
        df["parametersJsonSchema"] = structValue(std::move(*schema_or));
      }
      *function_decls.add_values() = structValue(std::move(decl));
      break;
    }
    case ToolKind::GoogleSearch: {
      Struct entry;
      (*entry.mutable_fields())["googleSearch"] = structValue(Struct{});
      *tools_list.add_values() = structValue(std::move(entry));
      break;
    }
    case ToolKind::EnterpriseWebSearch: {
      Struct entry;
      (*entry.mutable_fields())["enterpriseWebSearch"] = structValue(Struct{});
      *tools_list.add_values() = structValue(std::move(entry));
      break;
    }
    }
  }
  if (function_decls.values_size() > 0) {
    Struct fn_entry;
    (*fn_entry.mutable_fields())["functionDeclarations"] =
        listValue(std::move(function_decls));
    *tools_list.add_values() = structValue(std::move(fn_entry));
  }
  if (tools_list.values_size() > 0) {
    (*root_fields_parent.mutable_fields())["tools"] = listValue(std::move(tools_list));
  }
  return absl::OkStatus();
}

// §2.3 — toolConfig.functionCallingConfig.
void encodeToolChoice(const ToolChoice& tc, Struct& root_fields_parent) {
  if (tc.mode == ToolChoice::Mode::Unset) {
    return;
  }
  Struct fc_cfg;
  auto& fc_fields = *fc_cfg.mutable_fields();
  switch (tc.mode) {
  case ToolChoice::Mode::Auto:
    fc_fields["mode"] = stringValue("AUTO");
    break;
  case ToolChoice::Mode::None:
    fc_fields["mode"] = stringValue("NONE");
    break;
  case ToolChoice::Mode::Required:
    fc_fields["mode"] = stringValue("ANY");
    break;
  case ToolChoice::Mode::NamedFunction: {
    fc_fields["mode"] = stringValue("ANY");
    ListValue names;
    *names.add_values() = stringValue(tc.function_name);
    fc_fields["allowedFunctionNames"] = listValue(std::move(names));
    break;
  }
  case ToolChoice::Mode::Unset:
    return;
  }
  Struct tool_cfg;
  (*tool_cfg.mutable_fields())["functionCallingConfig"] = structValue(std::move(fc_cfg));
  (*root_fields_parent.mutable_fields())["toolConfig"] = structValue(std::move(tool_cfg));
}

// §2.4 + guided_* rows — writes into generationConfig. Gated on the caller
// having already constructed gen_fields. Returns error when a json-schema
// string fails to parse.
absl::Status encodeResponseFormat(const ResponseFormat& rf, Struct& gen_cfg) {
  auto& gf = *gen_cfg.mutable_fields();
  switch (rf.kind) {
  case ResponseFormat::Kind::Unset:
    return absl::OkStatus();
  case ResponseFormat::Kind::Text:
    gf["responseMimeType"] = stringValue("text/plain");
    return absl::OkStatus();
  case ResponseFormat::Kind::JsonObject:
    gf["responseMimeType"] = stringValue("application/json");
    return absl::OkStatus();
  case ResponseFormat::Kind::JsonSchema: {
    gf["responseMimeType"] = stringValue("application/json");
    auto schema_or = parseJsonSchemaStruct(rf.schema_json);
    if (!schema_or.ok()) {
      return absl::InvalidArgumentError("response_format.json_schema.schema is not valid JSON");
    }
    // Phase 3e: gate parametersJsonSchema vs. ResponseSchema on model version.
    // For now we always emit responseJsonSchema (Gemini 2.5+ path).
    gf["responseJsonSchema"] = structValue(std::move(*schema_or));
    return absl::OkStatus();
  }
  case ResponseFormat::Kind::GuidedChoice: {
    gf["responseMimeType"] = stringValue("text/x.enum");
    Struct schema;
    auto& sf = *schema.mutable_fields();
    sf["type"] = stringValue("STRING");
    ListValue enums;
    for (const auto& c : rf.choices) {
      *enums.add_values() = stringValue(c);
    }
    sf["enum"] = listValue(std::move(enums));
    gf["responseSchema"] = structValue(std::move(schema));
    return absl::OkStatus();
  }
  case ResponseFormat::Kind::GuidedRegex: {
    gf["responseMimeType"] = stringValue("application/json");
    Struct schema;
    auto& sf = *schema.mutable_fields();
    sf["type"] = stringValue("STRING");
    sf["pattern"] = stringValue(rf.regex);
    gf["responseSchema"] = structValue(std::move(schema));
    return absl::OkStatus();
  }
  case ResponseFormat::Kind::GuidedJson: {
    gf["responseMimeType"] = stringValue("application/json");
    auto schema_or = parseJsonSchemaStruct(rf.schema_json);
    if (!schema_or.ok()) {
      return absl::InvalidArgumentError("guided_json is not valid JSON schema");
    }
    gf["responseJsonSchema"] = structValue(std::move(*schema_or));
    return absl::OkStatus();
  }
  }
  return absl::OkStatus();
}

} // namespace

absl::Status GeminiEncoder::encode(const AiRequest& req, Buffer::Instance& out) {
  const InferencePayload* payload = req.asInference();
  if (payload == nullptr) {
    return absl::InvalidArgumentError("GeminiEncoder expects an Inference payload");
  }

  Struct root;
  auto& fields = *root.mutable_fields();

  // contents[]
  if (!payload->chat.empty()) {
    ListValue contents;
    for (const auto& m : payload->chat) {
      *contents.add_values() = structValue(buildContent(geminiRoleFor(m.role), m.text));
    }
    fields["contents"] = listValue(std::move(contents));
  }

  // systemInstruction — merged from all system/developer messages.
  if (!payload->system_instructions.empty()) {
    std::string merged;
    for (const auto& s : payload->system_instructions) {
      if (!merged.empty()) {
        merged += "\n";
      }
      merged += s;
    }
    // systemInstruction in Gemini uses the Content shape (role + parts).
    // Role is typically "user" or omitted; matching the Go translator's use
    // of a Content-with-role-user body.
    fields["systemInstruction"] = structValue(buildContent("user", merged));
  }

  // generationConfig
  Struct gen_cfg;
  auto& gen_fields = *gen_cfg.mutable_fields();
  if (payload->sampling.temperature.has_value()) {
    gen_fields["temperature"] = numberValue(*payload->sampling.temperature);
  }
  if (payload->sampling.top_p.has_value()) {
    gen_fields["topP"] = numberValue(*payload->sampling.top_p);
  }
  if (payload->sampling.max_tokens.has_value()) {
    gen_fields["maxOutputTokens"] = numberValue(static_cast<double>(*payload->sampling.max_tokens));
  }
  if (payload->sampling.n.has_value()) {
    gen_fields["candidateCount"] = numberValue(static_cast<double>(*payload->sampling.n));
  }
  if (payload->sampling.seed.has_value()) {
    gen_fields["seed"] = numberValue(static_cast<double>(*payload->sampling.seed));
  }
  if (payload->sampling.frequency_penalty.has_value()) {
    gen_fields["frequencyPenalty"] = numberValue(*payload->sampling.frequency_penalty);
  }
  if (payload->sampling.presence_penalty.has_value()) {
    gen_fields["presencePenalty"] = numberValue(*payload->sampling.presence_penalty);
  }
  if (!payload->sampling.stop.empty()) {
    ListValue stops;
    for (const auto& s : payload->sampling.stop) {
      *stops.add_values() = stringValue(s);
    }
    gen_fields["stopSequences"] = listValue(std::move(stops));
  }
  // Phase 3b — response_format / guided_* fold into generationConfig before
  // we seal it, so responseMimeType / responseSchema / responseJsonSchema all
  // live in the same object the Go translator writes them to.
  if (auto st = encodeResponseFormat(payload->response_format, gen_cfg); !st.ok()) {
    return st;
  }
  // Re-query size after encodeResponseFormat may have added fields.
  if (!gen_cfg.fields().empty()) {
    fields["generationConfig"] = structValue(std::move(gen_cfg));
  }

  // Phase 3b — tools[] and toolConfig. Both sit at the root of the body.
  if (auto st = encodeTools(payload->tools, root); !st.ok()) {
    return st;
  }
  encodeToolChoice(payload->tool_choice, root);

  // Not yet implemented (tracked in DESIGN.md): Phase 3c multimodal parts,
  // Phase 3d thinkingConfig / reasoningEffort, Phase 3e safety_settings and
  // vendor-field overrides.
  (void)boolValue;

  const std::string json = MessageUtil::getJsonStringFromMessageOrError(
      root, /*pretty_print=*/false, /*always_print_primitive_fields=*/false);
  out.add(json);
  return absl::OkStatus();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
