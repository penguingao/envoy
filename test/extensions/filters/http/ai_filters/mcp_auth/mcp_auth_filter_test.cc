// Functional tests for McpAuthFilter.
//
// McpAuthFilter is an AiFilter that runs inside AgenticChain (phase Q1).
// These tests exercise it directly through AiFilterChain::runRequestMetadata()
// using real Http::TestRequestHeaderMapImpl headers and AiRequest objects,
// without standing up the full Envoy HTTP stack. This is the natural boundary
// for a sub-chain filter — analogous to how McpFilterTest exercises McpFilter
// directly rather than through HttpIntegrationTest.
//
// Auth behaviors tested:
//   1.  Allow-listed method ("initialize") bypasses auth entirely.
// (see bottom of file for param_conditions tests 24–30)
//   2.  Missing identity header → 401 JSON-RPC error, no upstream.
//   3.  Valid identity header → Continue + mcp.principal populated.
//   4.  Admin-prefix method + non-admin principal → 403 JSON-RPC error.       [deprecated path]
//   5.  Admin-prefix method + "admin" principal → Continue.                   [deprecated path]
//   6.  Custom allow-listed method added to config → bypasses auth.
//   7.  Non-admin principal on a regular method → Continue (not an admin route).
//   8.  JSON-RPC id is echoed in error body.
//   9.  Notification (no id) → error body omits "id" field.
//   10. Custom identity header name.
//   11. Empty admin_method_prefix disables the admin check.                   [deprecated path]
//
// method_policies tests (new rule engine):
//   12. Exact-match policy: permitted principal passes.
//   13. Exact-match policy: non-permitted principal → 403.
//   14. Prefix-match policy ("admin/*"): permitted principal passes.
//   15. Prefix-match policy: non-permitted principal → 403.
//   16. Wildcard principal ("*"): any authenticated principal passes.
//   17. Empty allowed_principals: deny all authenticated principals.
//   18. First-match-wins: first rule allows, second would deny.
//   19. First-match-wins: first rule denies before a later allow-all rule.
//   20. No matching rule: authenticated principal allowed by default.
//   21. method_policies supersedes admin_method_prefix entirely.
//   22. 403 error body names both method and principal.
//   23. method_policies + notification (no id): error body omits "id".

#include "source/extensions/filters/http/ai_filters/mcp_auth/config.h"
#include "source/extensions/filters/http/ai_filters/mcp_auth/filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {
namespace {

using testing::HasSubstr;

// Outcome of a single runRequestMetadata() call.
struct ChainOutcome {
  bool ready{false};        // on_ready fired (request may proceed to upstream)
  bool local_reply{false};  // on_local_reply fired (request rejected)
  int  http_status{0};
  std::string body;
};

class McpAuthFilterTest : public testing::Test {
protected:
  // Build an AiFilterChain with a single McpAuthFilter using the given config.
  // Defaults to the zero-arg McpAuthFilterConfig (identity_header="x-mcp-identity",
  // allowed_unauthenticated_methods={"initialize"}, admin_method_prefix="admin/").
  std::unique_ptr<AiProtocolManager::Chain::AiFilterChain>
  makeChain(std::shared_ptr<const McpAuthFilterConfig> cfg = nullptr) {
    if (!cfg) {
      cfg = std::make_shared<McpAuthFilterConfig>();
    }
    auto chain = std::make_unique<AiProtocolManager::Chain::AiFilterChain>();
    chain->addAiFilter(std::make_unique<McpAuthFilter>(cfg));
    chain->finalizeInterests();
    return chain;
  }

  // Build an AiRequest with the given rpc_method, optional header map pointer,
  // and optional JSON-RPC id (empty = notification).
  AiProtocolManager::Codec::AiRequest
  makeRequest(std::string method, Http::RequestHeaderMap* headers = nullptr,
              std::string jsonrpc_id = "42") {
    AiProtocolManager::Codec::AiRequest req;
    req.rpc_method  = std::move(method);
    req.jsonrpc_id  = std::move(jsonrpc_id);
    req.headers     = headers;
    req.protocol    = AiProtocolManager::Codec::ProtocolKind::AgenticMcp;
    return req;
  }

  // Build an AiRequest that also carries an AgentPayload (for param_conditions tests).
  AiProtocolManager::Codec::AiRequest
  makeAgentRequest(std::string method, Http::RequestHeaderMap* headers,
                   AiProtocolManager::Codec::AgentPayload agent_payload,
                   std::string jsonrpc_id = "42") {
    AiProtocolManager::Codec::AiRequest req;
    req.rpc_method = std::move(method);
    req.jsonrpc_id = std::move(jsonrpc_id);
    req.headers    = headers;
    req.protocol   = AiProtocolManager::Codec::ProtocolKind::AgenticMcp;
    req.payload    = std::move(agent_payload);
    return req;
  }

  // Run the chain and return the outcome.
  ChainOutcome run(AiProtocolManager::Chain::AiFilterChain& chain,
                   AiProtocolManager::Codec::AiRequest& req) {
    ChainOutcome out;
    chain.runRequestMetadata(
        req,
        [&out](AiProtocolManager::Codec::AiRequest&) { out.ready = true; },
        [&out](AiProtocolManager::Codec::AiResponse&& resp) {
          out.local_reply = true;
          out.http_status = resp.http_status;
          out.body        = std::move(resp.body);
        });
    return out;
  }
};

// ── 1. Allow-listed method bypasses auth ─────────────────────────────────────

TEST_F(McpAuthFilterTest, InitializeMethodBypassesAuth) {
  auto chain = makeChain();
  // No identity header — would normally cause a 401. But "initialize" is
  // always in the allow-list so auth is skipped entirely.
  auto req = makeRequest("initialize", nullptr);
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 2. Missing identity header → 401 ─────────────────────────────────────────

TEST_F(McpAuthFilterTest, MissingIdentityHeaderRejects401) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {":method", "POST"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"},
      {"content-type", "application/json"}};
  auto req = makeRequest("tools/call", &headers, "7");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("\"jsonrpc\":\"2.0\""));
  EXPECT_THAT(out.body, HasSubstr("-32001"));
  EXPECT_THAT(out.body, HasSubstr("Unauthorized"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"7\""));
}

TEST_F(McpAuthFilterTest, NullHeaderMapRejects401) {
  auto chain = makeChain();
  // headers == nullptr — no header map at all.
  auto req = makeRequest("resources/read", nullptr, "1");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32001"));
}

// ── 3. Valid identity header → Continue + principal propagated ────────────────

TEST_F(McpAuthFilterTest, ValidIdentityHeaderPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {":method", "POST"}, {"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/list", &headers, "2");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  // Principal stored for downstream filters.
  EXPECT_EQ("alice", req.attributes.at("mcp.principal"));
}

// ── 4. Admin-prefix method + non-admin principal → 403 ───────────────────────

TEST_F(McpAuthFilterTest, AdminMethodWithNonAdminPrincipalRejects403) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "bob"}};
  auto req = makeRequest("admin/restart", &headers, "3");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
  EXPECT_THAT(out.body, HasSubstr("Forbidden"));
  EXPECT_THAT(out.body, HasSubstr("admin/restart"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"3\""));
}

// ── 5. Admin-prefix method + "admin" principal → Continue ────────────────────

TEST_F(McpAuthFilterTest, AdminMethodWithAdminPrincipalPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "admin"}};
  auto req = makeRequest("admin/restart", &headers, "4");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("admin", req.attributes.at("mcp.principal"));
}

// ── 6. Custom allow-listed method bypasses auth ───────────────────────────────

TEST_F(McpAuthFilterTest, CustomAllowListedMethodBypasses) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->allowed_unauthenticated_methods.insert("ping");  // add custom entry
  auto chain = makeChain(cfg);

  // No identity header — would normally cause a 401, but "ping" is allowed.
  auto req = makeRequest("ping", nullptr, "5");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 7. Non-admin principal on a regular method → Continue ─────────────────────

TEST_F(McpAuthFilterTest, NonAdminPrincipalOnRegularMethodPasses) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{
      {"x-mcp-identity", "charlie"}};
  auto req = makeRequest("resources/list", &headers, "6");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("charlie", req.attributes.at("mcp.principal"));
}

// ── 8. JSON-RPC id echoed in error body ──────────────────────────────────────

TEST_F(McpAuthFilterTest, JsonRpcIdEchoedIn401Body) {
  auto chain = makeChain();
  Http::TestRequestHeaderMapImpl headers{{":method", "POST"}};
  // Deliberate string id with special chars to confirm exact echo.
  auto req = makeRequest("prompts/get", &headers, "req-abc-123");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"req-abc-123\""));
}

// ── 9. Notification (empty id) → error body omits "id" field ─────────────────

TEST_F(McpAuthFilterTest, NotificationErrorBodyOmitsId) {
  auto chain = makeChain();
  // No headers → 401. Empty jsonrpc_id → "id" omitted per JSON-RPC 2.0 spec.
  auto req = makeRequest("tools/call", nullptr, "" /*notification*/);
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32001"));
  // The "id" key must not appear at all in the error body.
  EXPECT_THAT(out.body, testing::Not(HasSubstr("\"id\"")));
}

// ── 10. Custom identity header name ──────────────────────────────────────────

TEST_F(McpAuthFilterTest, CustomIdentityHeaderName) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->identity_header = "x-api-key";
  auto chain = makeChain(cfg);

  // Default "x-mcp-identity" is not present; custom "x-api-key" IS present.
  Http::TestRequestHeaderMapImpl headers{{"x-api-key", "service-account-1"}};
  auto req = makeRequest("tools/call", &headers, "8");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("service-account-1", req.attributes.at("mcp.principal"));
}

TEST_F(McpAuthFilterTest, CustomIdentityHeaderNameMissingRejects) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->identity_header = "x-api-key";
  auto chain = makeChain(cfg);

  // Old default header is present but the filter looks for "x-api-key".
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/call", &headers, "9");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(401, out.http_status);
}

// ── 11. Empty admin_method_prefix disables the admin check ───────────────────

TEST_F(McpAuthFilterTest, EmptyAdminPrefixDisablesAdminCheck) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";  // disabled
  auto chain = makeChain(cfg);

  // Any principal can call "admin/op" when the prefix check is disabled.
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "regular-user"}};
  auto req = makeRequest("admin/op", &headers, "10");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ═══════════════════════════════════════════════════════════════════════════════
// method_policies tests
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: build a config with a single MethodPolicy and default everything else.
static std::shared_ptr<McpAuthFilterConfig>
makePolicyConfig(std::string pattern,
                 absl::flat_hash_set<std::string> principals) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = ""; // disable deprecated path — policies take over
  MethodPolicy p;
  p.method_pattern = std::move(pattern);
  p.allowed_principals = std::move(principals);
  cfg->method_policies.push_back(std::move(p));
  return cfg;
}

// ── 12. Exact-match policy: permitted principal passes ───────────────────────

TEST_F(McpAuthFilterTest, MethodPolicyExactMatchAllowsPrincipal) {
  auto cfg = makePolicyConfig("tools/call", {"alice", "bob"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/call", &headers, "12");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
  EXPECT_EQ("alice", req.attributes.at("mcp.principal"));
}

// ── 13. Exact-match policy: non-permitted principal → 403 ───────────────────

TEST_F(McpAuthFilterTest, MethodPolicyExactMatchDeniesOtherPrincipal) {
  auto cfg = makePolicyConfig("tools/call", {"alice"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  auto req = makeRequest("tools/call", &headers, "13");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
  EXPECT_THAT(out.body, HasSubstr("Forbidden"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"13\""));
}

// ── 14. Prefix-match policy: permitted principal passes ──────────────────────

TEST_F(McpAuthFilterTest, MethodPolicyPrefixMatchAllowsPrincipal) {
  auto cfg = makePolicyConfig("admin/*", {"ops", "admin"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "ops"}};
  auto req = makeRequest("admin/shutdown", &headers, "14");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 15. Prefix-match policy: non-permitted principal → 403 ──────────────────

TEST_F(McpAuthFilterTest, MethodPolicyPrefixMatchDeniesOtherPrincipal) {
  auto cfg = makePolicyConfig("admin/*", {"admin"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "regular"}};
  auto req = makeRequest("admin/restart", &headers, "15");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
}

// ── 16. Wildcard principal ("*"): any authenticated principal passes ─────────

TEST_F(McpAuthFilterTest, MethodPolicyWildcardPrincipalAllowsAll) {
  auto cfg = makePolicyConfig("tools/*", {"*"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "anyone"}};
  auto req = makeRequest("tools/list", &headers, "16");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 17. Empty allowed_principals: deny all authenticated principals ──────────

TEST_F(McpAuthFilterTest, MethodPolicyEmptyPrincipalsDenyAll) {
  auto cfg = makePolicyConfig("debug/*", {} /*empty — deny all*/);
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "admin"}};
  auto req = makeRequest("debug/heap", &headers, "17");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
}

// ── 18. First-match-wins: first rule allows before a later deny ──────────────

TEST_F(McpAuthFilterTest, MethodPolicyFirstMatchWinsAllow) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  // Rule 1: tools/call → alice allowed
  MethodPolicy p1;
  p1.method_pattern = "tools/call";
  p1.allowed_principals = {"alice"};
  // Rule 2: tools/* → nobody allowed (would deny alice)
  MethodPolicy p2;
  p2.method_pattern = "tools/*";
  p2.allowed_principals = {};
  cfg->method_policies = {std::move(p1), std::move(p2)};

  auto chain = makeChain(cfg);
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/call", &headers, "18");
  auto out = run(*chain, req);

  // Rule 1 matched first and allowed alice — rule 2 is never evaluated.
  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 19. First-match-wins: first rule denies before a later allow-all ─────────

TEST_F(McpAuthFilterTest, MethodPolicyFirstMatchWinsDeny) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  // Rule 1: tools/call → nobody allowed
  MethodPolicy p1;
  p1.method_pattern = "tools/call";
  p1.allowed_principals = {};
  // Rule 2: tools/* → everyone allowed (would pass alice)
  MethodPolicy p2;
  p2.method_pattern = "tools/*";
  p2.allowed_principals = {"*"};
  cfg->method_policies = {std::move(p1), std::move(p2)};

  auto chain = makeChain(cfg);
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/call", &headers, "19");
  auto out = run(*chain, req);

  // Rule 1 matched first and denied — rule 2 is never evaluated.
  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
}

// ── 20. No matching rule: authenticated principal allowed by default ──────────

TEST_F(McpAuthFilterTest, MethodPolicyNoMatchDefaultAllow) {
  // Policy only covers "admin/*"; tools/* has no rule.
  auto cfg = makePolicyConfig("admin/*", {"admin"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  auto req = makeRequest("tools/list", &headers, "20");
  auto out = run(*chain, req);

  // No rule matched — default is allow for any authenticated principal.
  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 21. method_policies supersedes admin_method_prefix ───────────────────────
//
// When method_policies is non-empty, admin_method_prefix must be ignored even
// if it would have rejected the request.

TEST_F(McpAuthFilterTest, MethodPoliciesSupersedeAdminMethodPrefix) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  // admin_method_prefix is set and would normally reject "bob" for admin/*.
  cfg->admin_method_prefix = "admin/";
  // method_policies explicitly allows "*" for admin/* — takes precedence.
  MethodPolicy p;
  p.method_pattern = "admin/*";
  p.allowed_principals = {"*"};
  cfg->method_policies.push_back(std::move(p));

  auto chain = makeChain(cfg);
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "bob"}};
  auto req = makeRequest("admin/restart", &headers, "21");
  auto out = run(*chain, req);

  // method_policies allowed "*" → admin_method_prefix must have been ignored.
  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 22. 403 error body names both the method and the principal ───────────────

TEST_F(McpAuthFilterTest, MethodPolicy403BodyNamesMethodAndPrincipal) {
  auto cfg = makePolicyConfig("tools/call", {"alice"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  auto req = makeRequest("tools/call", &headers, "22");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("tools/call"));
  EXPECT_THAT(out.body, HasSubstr("eve"));
  EXPECT_THAT(out.body, HasSubstr("\"id\":\"22\""));
}

// ═══════════════════════════════════════════════════════════════════════════════
// param_conditions tests
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: build a MethodPolicy with one ParamCondition.
static MethodPolicy makePolicyWithParam(std::string method_pattern,
                                        absl::flat_hash_set<std::string> principals,
                                        ParamField field, StringMatcher matcher) {
  MethodPolicy p;
  p.method_pattern      = std::move(method_pattern);
  p.allowed_principals  = std::move(principals);
  ParamCondition cond;
  cond.field   = field;
  cond.matcher = std::move(matcher);
  p.param_conditions.push_back(std::move(cond));
  return p;
}

// ── 24. tools/call + tool_name exact match → permitted principal passes ───────

TEST_F(McpAuthFilterTest, ParamConditionToolNameExactAllows) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  cfg->method_policies.push_back(makePolicyWithParam(
      "tools/call", {"alice"},
      ParamField::ToolName, {StringMatcher::Kind::Exact, "search"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.tool_name = "search";
  auto req = makeAgentRequest("tools/call", &headers, std::move(agent), "24");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 25. tools/call + tool_name exact match → non-permitted principal → 403 ───

TEST_F(McpAuthFilterTest, ParamConditionToolNameExactDeniesOtherPrincipal) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  cfg->method_policies.push_back(makePolicyWithParam(
      "tools/call", {"alice"},
      ParamField::ToolName, {StringMatcher::Kind::Exact, "search"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.tool_name = "search";
  auto req = makeAgentRequest("tools/call", &headers, std::move(agent), "25");
  auto out = run(*chain, req);

  EXPECT_FALSE(out.ready);
  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
}

// ── 26. tools/call + wrong tool_name → condition misses → default allow ──────
//
// The policy only fires when tool_name == "search". A request with
// tool_name == "delete" hits no rule and falls through to the default allow.

TEST_F(McpAuthFilterTest, ParamConditionToolNameMismatchFallsThrough) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  cfg->method_policies.push_back(makePolicyWithParam(
      "tools/call", {"alice"},
      ParamField::ToolName, {StringMatcher::Kind::Exact, "search"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.tool_name = "delete"; // does not match — policy is skipped
  auto req = makeAgentRequest("tools/call", &headers, std::move(agent), "26");
  auto out = run(*chain, req);

  // No rule matched → default allow for any authenticated principal.
  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 27. resources/read + resource_uri prefix match → passes ──────────────────

TEST_F(McpAuthFilterTest, ParamConditionResourceUriPrefixAllows) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  cfg->method_policies.push_back(makePolicyWithParam(
      "resources/read", {"*"},
      ParamField::ResourceUri, {StringMatcher::Kind::Prefix, "public/"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "anyone"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.resource_uri = "public/readme";
  auto req = makeAgentRequest("resources/read", &headers, std::move(agent), "27");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 28. resources/read + resource_uri prefix mismatch → default allow ────────

TEST_F(McpAuthFilterTest, ParamConditionResourceUriPrefixMismatchFallsThrough) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  // Only policy covers public/ URIs; private/ has no matching rule.
  cfg->method_policies.push_back(makePolicyWithParam(
      "resources/read", {"alice"},
      ParamField::ResourceUri, {StringMatcher::Kind::Prefix, "public/"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.resource_uri = "private/secrets"; // prefix mismatch → policy skipped
  auto req = makeAgentRequest("resources/read", &headers, std::move(agent), "28");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 29. prompts/get + prompt_name suffix match → passes ──────────────────────

TEST_F(McpAuthFilterTest, ParamConditionPromptNameSuffixAllows) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";
  cfg->method_policies.push_back(makePolicyWithParam(
      "prompts/get", {"alice"},
      ParamField::PromptName, {StringMatcher::Kind::Suffix, "-public"}));
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "alice"}};
  AiProtocolManager::Codec::AgentPayload agent;
  agent.prompt_name = "onboarding-public";
  auto req = makeAgentRequest("prompts/get", &headers, std::move(agent), "29");
  auto out = run(*chain, req);

  EXPECT_TRUE(out.ready);
  EXPECT_FALSE(out.local_reply);
}

// ── 30. Multiple param_conditions — ALL must match ────────────────────────────
//
// Policy: tools/call, tool_name exact "search", only alice allowed.
// Request has tool_name "search" (condition 1 passes) but principal "eve"
// (not in allowed list) → 403.
// Also verifies a second request where tool_name is wrong → condition fails
// → policy skipped → default allow for eve.

TEST_F(McpAuthFilterTest, ParamConditionAllMustMatch) {
  auto cfg = std::make_shared<McpAuthFilterConfig>();
  cfg->admin_method_prefix = "";

  MethodPolicy p;
  p.method_pattern     = "tools/call";
  p.allowed_principals = {"alice"};
  ParamCondition c1;
  c1.field   = ParamField::ToolName;
  c1.matcher = {StringMatcher::Kind::Exact, "search"};
  p.param_conditions.push_back(c1);
  cfg->method_policies.push_back(std::move(p));

  auto chain = makeChain(cfg);
  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};

  // Both conditions met (tool matches, principal wrong) → 403.
  {
    AiProtocolManager::Codec::AgentPayload agent;
    agent.tool_name = "search";
    auto req = makeAgentRequest("tools/call", &headers, std::move(agent), "30a");
    auto out = run(*chain, req);
    EXPECT_EQ(403, out.http_status);
  }

  // Tool name doesn't match → policy skipped → default allow.
  {
    AiProtocolManager::Codec::AgentPayload agent;
    agent.tool_name = "other";
    auto req = makeAgentRequest("tools/call", &headers, std::move(agent), "30b");
    auto out = run(*chain, req);
    EXPECT_TRUE(out.ready);
  }
}

// ── 23. method_policies + notification (no id) → error body omits "id" ───────

TEST_F(McpAuthFilterTest, MethodPolicyNotificationErrorOmitsId) {
  auto cfg = makePolicyConfig("tools/call", {"alice"});
  auto chain = makeChain(cfg);

  Http::TestRequestHeaderMapImpl headers{{"x-mcp-identity", "eve"}};
  auto req = makeRequest("tools/call", &headers, "" /*notification*/);
  auto out = run(*chain, req);

  EXPECT_TRUE(out.local_reply);
  EXPECT_EQ(403, out.http_status);
  EXPECT_THAT(out.body, HasSubstr("-32003"));
  EXPECT_THAT(out.body, testing::Not(HasSubstr("\"id\"")));
}

} // namespace
} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
