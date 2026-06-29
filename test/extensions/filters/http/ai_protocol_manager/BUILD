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
    name = "ai_protocol_manager_test",
    srcs = ["ai_protocol_manager_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager:ai_protocol_manager_lib",
        "//test/mocks/http:http_mocks",
    ],
)

envoy_extension_cc_test(
    name = "ai_protocol_manager_integration_test",
    size = "large",
    srcs = ["ai_protocol_manager_integration_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager:config",
        "//test/integration:http_protocol_integration_lib",
        "//test/mocks/http:http_mocks",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
    ],
)
