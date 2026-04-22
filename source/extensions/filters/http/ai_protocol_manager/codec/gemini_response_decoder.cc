#include "source/extensions/filters/http/ai_protocol_manager/codec/gemini_response_decoder.h"

#include <cstdint>
#include <string>

#include "source/common/json/json_loader.h"
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

// OPENAI_VERTEX_SPEC.md §4.3 — finishReason mapping.
std::string mapFinishReason(absl::string_view gemini_reason, bool has_tool_calls) {
  if (gemini_reason == "STOP") {
    return has_tool_calls ? "tool_calls" : "stop";
  }
  if (gemini_reason == "MAX_TOKENS") {
    return "length";
  }
  if (gemini_reason.empty()) {
    return "";
  }
  // SAFETY, RECITATION, BLOCKLIST, PROHIBITED_CONTENT, ... — bucket as
  // content_filter per the Go translator's default fallback.
  return "content_filter";
}

// Walk a Gemini candidate.content.parts[] and split it into:
//   - concatenated visible text (Thought==false)
//   - tool_calls list
// Reasoning (Thought==true) is dropped for V0; OpenAI's reasoning_content
// field comes back when we extend the InferenceResponseSummary.
struct CandidateBody {
  std::string content_text;
  ListValue tool_calls;
};
CandidateBody walkParts(const Json::Object& candidate, int candidate_index) {
  CandidateBody out;
  auto content_or = candidate.getObject("content", /*allow_empty=*/true);
  if (!content_or.ok() || *content_or == nullptr) {
    return out;
  }
  auto parts_or = (*content_or)->getObjectArray("parts", /*allow_empty=*/true);
  if (!parts_or.ok()) {
    return out;
  }
  int tc_idx = 0;
  for (const auto& part : *parts_or) {
    const bool is_thought = part->getBoolean("thought", false).value_or(false);

    // Text part — include unless it is a thought trace (deferred).
    if (!is_thought) {
      if (auto t = part->getString("text"); t.ok()) {
        out.content_text += *t;
      }
    }

    // FunctionCall part → tool_call entry.
    if (part->hasObject("functionCall")) {
      auto fc_or = part->getObject("functionCall", /*allow_empty=*/true);
      if (!fc_or.ok() || *fc_or == nullptr) {
        continue;
      }
      const auto& fc = *fc_or;
      Struct tc;
      auto& tcf = *tc.mutable_fields();
      // Synthesize a stable id from candidate + per-candidate position so a
      // single Gemini response with several function calls round-trips
      // without colliding ids.
      tcf["id"] = stringValue(absl::StrCat("call_", candidate_index, "_", tc_idx));
      tcf["type"] = stringValue("function");

      Struct fn;
      auto& ff = *fn.mutable_fields();
      ff["name"] = stringValue(fc->getString("name", "").value_or(""));
      // Gemini's args is a JSON object; OpenAI's arguments is a JSON-encoded
      // string. asJsonString() round-trips it to the OpenAI shape verbatim.
      auto args_or = fc->getObject("args", /*allow_empty=*/true);
      if (args_or.ok() && *args_or != nullptr) {
        ff["arguments"] = stringValue((*args_or)->asJsonString());
      } else {
        ff["arguments"] = stringValue("{}");
      }
      tcf["function"] = structValue(std::move(fn));
      *out.tool_calls.add_values() = structValue(std::move(tc));
      ++tc_idx;
    }
  }
  return out;
}

// OPENAI_VERTEX_SPEC.md §4.1 — one candidate → one OpenAI choice.
Struct convertCandidate(const Json::Object& candidate, int index) {
  Struct choice;
  auto& cf = *choice.mutable_fields();
  cf["index"] = numberValue(index);

  CandidateBody body = walkParts(candidate, index);
  const bool has_tool_calls = body.tool_calls.values_size() > 0;

  Struct message;
  auto& mf = *message.mutable_fields();
  mf["role"] = stringValue("assistant");
  if (has_tool_calls) {
    // §9 invariant: tool_calls and content are mutually exclusive in the
    // OpenAI response shape — if tool calls are present, content is null.
    mf["tool_calls"] = listValue(std::move(body.tool_calls));
  } else {
    mf["content"] = stringValue(body.content_text);
  }
  cf["message"] = structValue(std::move(message));

  const std::string finish =
      mapFinishReason(candidate.getString("finishReason", "").value_or(""), has_tool_calls);
  if (!finish.empty()) {
    cf["finish_reason"] = stringValue(finish);
  }
  return choice;
}

// OPENAI_VERTEX_SPEC.md §4.2 — usageMetadata → usage. Gemini's
// candidatesTokenCount and thoughtsTokenCount both count toward OpenAI's
// completion_tokens (a thought is generated output even if the wire
// shows it as a separate counter).
Struct convertUsage(const Json::Object& usage_metadata) {
  Struct usage;
  auto& uf = *usage.mutable_fields();
  const int64_t prompt = usage_metadata.getInteger("promptTokenCount", 0).value_or(0);
  const int64_t cand = usage_metadata.getInteger("candidatesTokenCount", 0).value_or(0);
  const int64_t thoughts = usage_metadata.getInteger("thoughtsTokenCount", 0).value_or(0);
  const int64_t total = usage_metadata.getInteger("totalTokenCount", 0).value_or(0);
  uf["prompt_tokens"] = numberValue(static_cast<double>(prompt));
  uf["completion_tokens"] = numberValue(static_cast<double>(cand + thoughts));
  uf["total_tokens"] = numberValue(static_cast<double>(total));
  return usage;
}

// Populate the typed AiResponse summary from the parsed Gemini body so that
// AiFilters running R3 onResponseEnd can read finish_reason / usage / model
// without re-parsing the wire body. Mirrors what the JSON encoder above
// produced.
void populateSummary(const Json::Object& root, const std::vector<Json::ObjectSharedPtr>& candidates,
                     AiResponse& ai_response) {
  auto& summary = ai_response.summary.emplace<InferenceResponseSummary>();
  summary.id = root.getString("responseId", "").value_or("");
  summary.model = root.getString("modelVersion", "").value_or("");

  if (!candidates.empty()) {
    const auto fr = candidates[0]->getString("finishReason", "").value_or("");
    // Approximation: detect tool_calls by walking parts of the first
    // candidate. Multi-candidate responses are rare for OpenAI-shape
    // clients (n=1 default).
    bool has_tool_calls = false;
    auto content_or = candidates[0]->getObject("content", /*allow_empty=*/true);
    if (content_or.ok() && *content_or != nullptr) {
      auto parts_or = (*content_or)->getObjectArray("parts", /*allow_empty=*/true);
      if (parts_or.ok()) {
        for (const auto& part : *parts_or) {
          if (part->hasObject("functionCall")) {
            has_tool_calls = true;
            break;
          }
        }
      }
    }
    summary.finish_reason = mapFinishReason(fr, has_tool_calls);
  }

  if (root.hasObject("usageMetadata")) {
    auto um_or = root.getObject("usageMetadata", /*allow_empty=*/true);
    if (um_or.ok() && *um_or != nullptr) {
      const auto& um = **um_or;
      if (auto v = um.getInteger("promptTokenCount"); v.ok()) {
        summary.usage.prompt_tokens = static_cast<int32_t>(*v);
      }
      const int64_t cand = um.getInteger("candidatesTokenCount", 0).value_or(0);
      const int64_t thoughts = um.getInteger("thoughtsTokenCount", 0).value_or(0);
      summary.usage.completion_tokens = static_cast<int32_t>(cand + thoughts);
      if (auto v = um.getInteger("totalTokenCount"); v.ok()) {
        summary.usage.total_tokens = static_cast<int32_t>(*v);
      }
    }
  }
}

} // namespace

absl::Status GeminiResponseDecoder::decodeFullBody(absl::string_view upstream_body,
                                                    AiResponse& ai_response,
                                                    Buffer::Instance& out_body) {
  if (upstream_body.empty()) {
    return absl::InvalidArgumentError("empty upstream body");
  }
  auto root_or = Json::Factory::loadFromString(std::string(upstream_body));
  if (!root_or.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("upstream body is not valid JSON: ", root_or.status().message()));
  }
  const auto& root = *root_or;

  Struct openai;
  auto& of = *openai.mutable_fields();
  of["id"] = stringValue(root->getString("responseId", "").value_or(""));
  of["object"] = stringValue("chat.completion");
  // Gemini's createTime is RFC3339; OpenAI's `created` is unix seconds.
  // Parsing RFC3339 in V0 isn't worth a date dep — emit 0 and let clients
  // ignore. Real-time accuracy is not required by any OpenAI consumer.
  of["created"] = numberValue(0);
  of["model"] = stringValue(root->getString("modelVersion", "").value_or(""));

  // candidates → choices.
  ListValue choices;
  std::vector<Json::ObjectSharedPtr> candidates;
  auto cand_or = root->getObjectArray("candidates", /*allow_empty=*/true);
  if (cand_or.ok()) {
    candidates = *cand_or;
    int idx = 0;
    for (const auto& cand : candidates) {
      *choices.add_values() = structValue(convertCandidate(*cand, idx));
      ++idx;
    }
  }
  of["choices"] = listValue(std::move(choices));

  // usage.
  if (root->hasObject("usageMetadata")) {
    auto um_or = root->getObject("usageMetadata", /*allow_empty=*/true);
    if (um_or.ok() && *um_or != nullptr) {
      of["usage"] = structValue(convertUsage(**um_or));
    }
  }

  populateSummary(*root, candidates, ai_response);

  const std::string json = MessageUtil::getJsonStringFromMessageOrError(
      openai, /*pretty_print=*/false, /*always_print_primitive_fields=*/false);
  out_body.add(json);
  return absl::OkStatus();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
