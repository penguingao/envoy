// Unit tests for RequestDecoder body-size tiering.
//
// Verifies the three-tier behavior introduced in InferenceBodyParser and
// AgentBodyParser:
//
//   Tier 1 — body ≤ max_element_capture_bytes:
//     Full byte-range element capture. For inference: messages[]/tools[] each
//     get a populated PayloadRef. For agents: params object captured in
//     params_raw and routing fields (tool_name, etc.) are populated.
//
//   Tier 2 — max_element_capture_bytes < body ≤ max_body_bytes:
//     Scalar-only extraction. Top-level primitives (model, stream, id, method,
//     sampling params) are extracted normally. Element/params capture is
//     skipped to avoid the 2× memory cost of copying large bytes out of the
//     slab chain.
//
//   Tier 3 — body > max_body_bytes:
//     Hard limit. onData() returns ResourceExhausted immediately, before the
//     buffer grows beyond the ceiling.
//
// Test matrix:
//
//   InferenceBodyParser
//     SmallBody_ElementsCaptured          body < soft limit → messages populated
//     LargeBody_ScalarsOnlyNoElements     body > soft limit → messages empty, scalars ok
//     ExceedsHardLimit_ReturnsError       body > hard limit → ResourceExhausted from onData
//
//   AgentBodyParser
//     SmallBody_ParamsCaptured            body < soft limit → params_raw + tool_name set
//     LargeBody_ScalarsOnlyNoParams       body > soft limit → params_raw empty, method set
//     ExceedsHardLimit_ReturnsError       body > hard limit → ResourceExhausted from onData

#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {
namespace {

// Thresholds chosen so that test bodies can trivially fall into any tier
// without synthesizing multi-megabyte payloads:
//   soft limit  = 100 bytes
//   hard limit  = 500 bytes
constexpr size_t kSoftLimit = 100;
constexpr size_t kHardLimit = 500;

DecoderConfig makeCfg() {
  DecoderConfig cfg;
  cfg.max_element_capture_bytes = kSoftLimit;
  cfg.max_body_bytes            = kHardLimit;
  return cfg;
}

// Drive RequestDecoder through its full lifecycle. Returns the completed
// AiRequest on success or a non-OK status on any failure.
absl::StatusOr<AiRequest> runDecoder(RequestDecoder&             dec,
                                     const Http::TestRequestHeaderMapImpl& hdrs,
                                     absl::string_view                     body) {
  if (auto s = dec.onHeaders(hdrs); !s.ok()) return s;
  if (!body.empty()) {
    if (auto s = dec.onData(body); !s.ok()) return s;
  }
  if (auto s = dec.onEndStream(); !s.ok()) return s;
  return dec.take();
}

// ─────────────────────────────────────────────────────────────────────────────
// InferenceBodyParser — three-tier tests
// ─────────────────────────────────────────────────────────────────────────────

class InferenceDecoderTest : public testing::Test {
protected:
  DecoderConfig        cfg_   = makeCfg();
  InMemoryPayloadStore store_;
  RequestDecoder       dec_{cfg_, store_};

  const Http::TestRequestHeaderMapImpl kInferenceHeaders{
      {":method",       "POST"},
      {":path",         "/v1/chat/completions"},
      {":authority",    "host"},
      {"content-type",  "application/json"},
  };
};

// Tier 1: body < soft limit → messages[] element captured.
TEST_F(InferenceDecoderTest, SmallBody_ElementsCaptured) {
  // ~58 bytes — well below kSoftLimit (100).
  const std::string body =
      R"({"model":"gpt-4o","stream":true,"max_tokens":512,)"
      R"("messages":[{"role":"user","content":"Hi"}]})";
  ASSERT_LT(body.size(), kSoftLimit);

  auto result = runDecoder(dec_, kInferenceHeaders, body);
  ASSERT_TRUE(result.ok()) << result.status();

  const auto* payload = result->as_inference();
  ASSERT_NE(nullptr, payload);

  // Scalars extracted.
  EXPECT_EQ("gpt-4o", payload->target.name);
  EXPECT_TRUE(result->streaming);
  EXPECT_EQ(512, payload->sampling.max_tokens);

  // Element captured: messages has exactly one entry.
  EXPECT_EQ(1u, payload->messages.size());
  EXPECT_TRUE(payload->tools.empty());
}

// Tier 2: body > soft limit → scalars extracted, messages[] NOT captured.
TEST_F(InferenceDecoderTest, LargeBody_ScalarsOnlyNoElements) {
  // Build a body whose message content pushes total size past kSoftLimit.
  // Prefix alone is ~57 bytes; 80-char content → ~141 bytes total.
  const std::string large_content(80, 'x');
  const std::string body =
      R"({"model":"gpt-4o","stream":true,)"
      R"("messages":[{"role":"user","content":")" +
      large_content + R"("}]})";
  ASSERT_GT(body.size(), kSoftLimit);
  ASSERT_LT(body.size(), kHardLimit);

  auto result = runDecoder(dec_, kInferenceHeaders, body);
  ASSERT_TRUE(result.ok()) << result.status();

  const auto* payload = result->as_inference();
  ASSERT_NE(nullptr, payload);

  // Scalars still extracted.
  EXPECT_EQ("gpt-4o", payload->target.name);
  EXPECT_TRUE(result->streaming);

  // Element capture skipped — messages is empty.
  EXPECT_TRUE(payload->messages.empty());
  EXPECT_TRUE(payload->tools.empty());
}

// Tier 3: body > hard limit → onData returns ResourceExhausted.
TEST_F(InferenceDecoderTest, ExceedsHardLimit_ReturnsError) {
  const std::string huge_content(kHardLimit + 1, 'x');
  const std::string body =
      R"({"model":"gpt-4o","messages":[{"role":"user","content":")" +
      huge_content + R"("}]})";
  ASSERT_GT(body.size(), kHardLimit);

  if (auto s = dec_.onHeaders(kInferenceHeaders); !s.ok()) {
    FAIL() << "onHeaders failed: " << s;
  }
  const absl::Status data_status = dec_.onData(body);
  EXPECT_FALSE(data_status.ok());
  EXPECT_EQ(absl::StatusCode::kResourceExhausted, data_status.code());
}

// ─────────────────────────────────────────────────────────────────────────────
// AgentBodyParser — three-tier tests
// ─────────────────────────────────────────────────────────────────────────────

class AgentDecoderTest : public testing::Test {
protected:
  DecoderConfig        cfg_   = makeCfg();
  InMemoryPayloadStore store_;
  RequestDecoder       dec_{cfg_, store_};

  // POST to a non-/v1/ path with application/json falls through to the MCP
  // fallback in the protocol classifier.
  const Http::TestRequestHeaderMapImpl kAgentHeaders{
      {":method",       "POST"},
      {":path",         "/mcp"},
      {":authority",    "host"},
      {"content-type",  "application/json"},
  };
};

// Tier 1: body < soft limit → params captured, routing fields populated.
TEST_F(AgentDecoderTest, SmallBody_ParamsCaptured) {
  // ~68 bytes — below kSoftLimit (100).
  const std::string body =
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
      R"("params":{"name":"search","arguments":{}}})";
  ASSERT_LT(body.size(), kSoftLimit);

  auto result = runDecoder(dec_, kAgentHeaders, body);
  ASSERT_TRUE(result.ok()) << result.status();

  const auto* payload = result->as_agent();
  ASSERT_NE(nullptr, payload);

  // Scalars extracted.
  EXPECT_EQ("tools/call", result->rpc_method);

  // params captured → params_raw populated and tool_name extracted.
  EXPECT_FALSE(payload->params_raw.empty());
  EXPECT_EQ("search", payload->tool_name);
}

// Tier 2: body > soft limit → scalars extracted, params NOT captured.
TEST_F(AgentDecoderTest, LargeBody_ScalarsOnlyNoParams) {
  // Prefix ~86 bytes; 80-char argument → ~171 bytes total.
  const std::string large_arg(80, 'y');
  const std::string body =
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
      R"("params":{"name":"search","arguments":{"query":")" +
      large_arg + R"("}}})";
  ASSERT_GT(body.size(), kSoftLimit);
  ASSERT_LT(body.size(), kHardLimit);

  auto result = runDecoder(dec_, kAgentHeaders, body);
  ASSERT_TRUE(result.ok()) << result.status();

  const auto* payload = result->as_agent();
  ASSERT_NE(nullptr, payload);

  // Top-level scalar method still extracted.
  EXPECT_EQ("tools/call", result->rpc_method);

  // Params capture skipped — params_raw empty, tool_name not populated.
  EXPECT_TRUE(payload->params_raw.empty());
  EXPECT_TRUE(payload->tool_name.empty());
}

// Tier 3: body > hard limit → onData returns ResourceExhausted.
TEST_F(AgentDecoderTest, ExceedsHardLimit_ReturnsError) {
  const std::string huge_arg(kHardLimit + 1, 'z');
  const std::string body =
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
      R"("params":{"arguments":{"data":")" +
      huge_arg + R"("}}})" ;
  ASSERT_GT(body.size(), kHardLimit);

  if (auto s = dec_.onHeaders(kAgentHeaders); !s.ok()) {
    FAIL() << "onHeaders failed: " << s;
  }
  const absl::Status data_status = dec_.onData(body);
  EXPECT_FALSE(data_status.ok());
  EXPECT_EQ(absl::StatusCode::kResourceExhausted, data_status.code());
}

} // namespace
} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
