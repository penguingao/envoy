load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "mcp_auth_filter_test",
    srcs = ["mcp_auth_filter_test.cc"],
    deps = [
        "//source/extensions/filters/http/ai_filters/mcp_auth:mcp_auth_filter_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
        "//test/test_common:utility_lib",
    ],
)

envoy_cc_test(
    name = "mcp_auth_filter_integration_test",
    srcs = ["mcp_auth_filter_integration_test.cc"],
    deps = [
        # Pull in the config registrations so the factories are available at runtime.
        "//source/extensions/filters/http/ai_protocol_manager:config",
        "//source/extensions/filters/http/ai_filters/mcp_auth:mcp_auth_filter_lib",
        "//test/integration:http_integration_lib",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/http/ai_filters/mcp_auth/v3:pkg_cc_proto",
    ],
)
