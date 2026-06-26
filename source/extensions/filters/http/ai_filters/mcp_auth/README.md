# McpAuthFilter Overview

`McpAuthFilter` is an `AiFilter` that runs as the first filter in the `AgenticChain` —
before any other agentic filter sees the request. It receives an already-decoded `AiRequest`
from `AiProtocolManager`; the JSON-RPC method name, id, HTTP headers, and MCP param fields
(`tool_name`, `resource_uri`, `prompt_name`) are fully extracted before this filter is
invoked, so it performs no body parsing of its own.

The filter enforces three ordered checks on every MCP request:

1. **Allow-list bypass** — methods in `allowed_unauthenticated_methods` (always includes
   `"initialize"`) skip all further checks and proceed immediately. This ensures the MCP
   handshake is reachable before a session is authenticated.

2. **Authentication** — reads the identity header (default `x-mcp-identity`). If absent,
   returns a JSON-RPC 2.0 `{"error":{"code":-32001}}` with HTTP 401. The request never
   reaches the upstream.

3. **Authorisation** — evaluates per-method access policies loaded from the config proto.
   Rules are evaluated in order; the first matching rule wins. A rule matches when:
   - the `method_pattern` matches the JSON-RPC method name (exact, or prefix ending with
     `"*"`), **and**
   - every `param_condition` holds — conditions inspect MCP-specific param fields:
     `tool_name` (for `tools/call`), `resource_uri` (for `resources/read`), or
     `prompt_name` (for `prompts/get`) — using exact, prefix, or suffix string matching.

   `allowed_principals` controls who may call matched methods; `"*"` permits any
   authenticated principal; an empty list denies all. Methods not matched by any rule
   default-allow any authenticated principal. On denial, returns a JSON-RPC 2.0
   `{"error":{"code":-32003}}` with HTTP 403. The deprecated `admin_method_prefix` field
   is used as a fallback only when `method_policies` is empty.

On success, the verified principal is stored in `request.attributes["mcp.principal"]` for
downstream `AiFilter`s (quota, audit, routing) to consume without re-parsing the header.

**Example policy** — per-tool access control inside `tools/call`:

```yaml
method_policies:
- method_pattern: "tools/call"
  allowed_principals: ["*"]
  param_conditions:
  - field: TOOL_NAME
    matcher:
      exact: "search"
- method_pattern: "tools/call"
  allowed_principals: ["admin"]
  param_conditions:
  - field: TOOL_NAME
    matcher:
      exact: "delete"
- method_pattern: "tools/call"   # catch-all for any other tool
  allowed_principals: ["*"]
- method_pattern: "resources/read"
  allowed_principals: ["*"]
  param_conditions:
  - field: RESOURCE_URI
    matcher:
      prefix: "public/"
- method_pattern: "resources/read"   # catch-all: only alice for private URIs
  allowed_principals: ["alice"]
- method_pattern: "admin/*"
  allowed_principals: ["admin", "ops"]
```