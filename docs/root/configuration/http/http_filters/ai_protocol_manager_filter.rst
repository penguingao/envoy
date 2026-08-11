.. _config_http_filters_ai_protocol_manager:

AI Protocol Manager
===================

The AI Protocol Manager filter (alpha) buffers the request payload off the
connection manager's hot path so that routing and admission decisions can be
made on the fully received body.

It does so only for requests it has a reason to inspect. A request on a route
that is not a declared AI endpoint -- and, unless :ref:`best_effort_parsing
<envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager.best_effort_parsing>`
is set, every request -- passes straight through: its headers are not held, its
body is not offloaded, and no external buffer is created for it. A filter chain
carrying this filter therefore costs ordinary pass-through for the traffic it
does not serve.

For a request it does inspect, as the body arrives the filter offloads it into an external buffer
rather than pinning it in the connection manager's in-memory buffers. Once the
stream ends, it streams the buffered bytes back into the filter chain so that
the subsequent filters observe the request unchanged. The offload/replay
round-trip is flow-controlled in both directions: ingest honors the buffer
limit, and replay is paced against filter-chain back-pressure, so the resident
footprint stays bounded regardless of payload size.

While such a body is being offloaded, the request headers are held at this filter
and released to the subsequent filters only once replay begins, so they never act
on the headers before the payload they depend on is available.

On a route declared to be an AI endpoint, the body is parsed as it is offloaded,
so that a payload which is not well-formed JSON is rejected here rather than
forwarded for the upstream to interpret differently. Parsing is incremental and shares the offload's byte
stream, so an invalid payload fails as soon as the offending byte arrives rather
than after the whole upload. Oversized string values are left in the external
buffer and referenced by offset, so a large prompt does not reappear in
per-stream memory.

Once the body has been received in full it is validated against the schema the
route declared, and a payload that does not conform is rejected. See `Request
validation`_ below.

.. note::

  Only the request (decode) path is wired today, and the body is offloaded to an
  in-memory store. Transcoding to the canonical schema is not implemented yet, so
  :ref:`normalize
  <envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManagerPerRoute.normalize>`
  is accepted and has no effect, and the response payload has no schema of its
  own yet.

The filter is a dual filter: besides the downstream HTTP filter chain shown
below, it can also be placed in a cluster's upstream HTTP filter chain via
:ref:`http_filters <envoy_v3_api_field_extensions.upstreams.http.v3.HttpProtocolOptions.http_filters>`,
where the offload/replay round-trip runs after load balancing and host
selection (and therefore once per retry or hedged attempt).

.. note::

  Two caveats apply to the upstream placement, and only to routes the filter
  inspects. The filter holds the request headers until the payload has been
  fully offloaded, and upstream filter
  chains have no per-upgrade-type chain selection (the
  :ref:`upgrade_configs <envoy_v3_api_field_extensions.filters.network.http_connection_manager.v3.HttpConnectionManager.upgrade_configs>`
  escape hatch is downstream-only), so the filter must not front upgrade or
  CONNECT routes, or other requests whose body does not end promptly: such
  streams would stall until the request times out. Additionally, local replies
  raised from an upstream filter chain (such as this filter's external-buffer
  error reply) are delivered directly to the downstream client without
  consulting the router's retry or hedging logic.

* This filter should be configured with the type URL ``type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager``.
* :ref:`v3 API reference <envoy_v3_api_msg_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager>`

Configuration
-------------

Which routes are AI endpoints is declared per route, with
:ref:`AiProtocolManagerPerRoute
<envoy_v3_api_msg_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManagerPerRoute>`.
A route that carries one names the :ref:`schema
<envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManagerPerRoute.schema>`
its payload follows, and its payload is parsed and validated strictly: a body
that is malformed or does not conform to that schema is rejected with a 400. This
is normally attached to a route matching the provider's REST path, such as
``/chat/completions``:

.. code-block:: yaml

  routes:
  - match:
      path: "/chat/completions"
    route:
      cluster: openai
    typed_per_filter_config:
      envoy.filters.http.ai_protocol_manager:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManagerPerRoute
        schema: OPENAI_CHAT_COMPLETIONS

Such a route is a pass-through endpoint: the payload is forwarded upstream in
its own schema. Setting :ref:`normalize
<envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManagerPerRoute.normalize>`
additionally transcodes it into the canonical schema, which is what lets one set
of filters operate on payloads from different providers.

The filter-level configuration decides what happens on every other route. By
default those requests are passed through untouched -- not parsed, and not
offloaded; setting
:ref:`best_effort_parsing
<envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager.best_effort_parsing>`
offloads and parses them too, but never fails a request over it -- a payload that
does not parse is forwarded unchanged.

.. code-block:: yaml

  http_filters:
  - name: envoy.filters.http.ai_protocol_manager
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
      best_effort_parsing: true

Request validation
------------------

A declared AI endpoint's payload is checked against the schema its route named,
once the body has been received in full.

**Only declared fields are constrained.** Any other field is forwarded untouched,
so a request using a provider field newer than this Envoy is not rejected for it.
The schemas also deliberately leave fast-moving values unconstrained -- the type
of ``service_tier`` is checked, for example, but not which value it holds --
because a proxy that rejects a request the upstream would have accepted is worse
than one that forwards a request the upstream rejects itself. Caller-authored JSON
Schema (``tools[].function.parameters``,
``response_format.json_schema.schema``) is carried through without being
interpreted at all.

A payload that violates the schema is answered with a 400 whose
``response_code_details`` is ``ai_protocol_manager_schema_violation`` --
distinct from the ``ai_protocol_manager_invalid_json`` a malformed body gets, so
the two are separable in stats and access logs. The response body names the
offending field and what was expected of it, for example:

.. code-block:: text

  messages[0].role: value not permitted

That message never echoes the value the client sent, so prompt content cannot
reach a response, an access log, or a stat.

A string value large enough to have been left in the external buffer has its type
checked but not its contents. This does not weaken anything the schemas
constrain: a field with a restricted set of values is never one that may be
offloaded, so such a value is always available to check.

A route that is not a declared AI endpoint is never validated, including under
:ref:`best_effort_parsing
<envoy_v3_api_field_extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager.best_effort_parsing>`:
it named no schema, so there is nothing to hold its payload to.

Limitations
-----------

JSON nesting deeper than 8 levels is rejected as malformed before validation
runs, and is reported as ``ai_protocol_manager_invalid_json`` rather than as a
depth problem. A deeply nested ``tools[].function.parameters`` JSON Schema can
reach that limit -- a tool argument that is itself an object of objects is close
to it -- so this is worth knowing before fronting tool-calling traffic.

