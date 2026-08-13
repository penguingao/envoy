load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "auto_router_test",
    srcs = ["auto_router_test.cc"],
    extension_names = ["envoy.filters.ai.auto_router"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/ai/auto_router:auto_router_lib",
        "//source/extensions/filters/http/ai_protocol_manager:json_with_ext_buf_lib",
        "//test/mocks/server:factory_context_mocks",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/ai/auto_router/v3:pkg_cc_proto",
    ],
)
