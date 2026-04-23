load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "mcp_auth_filter_lib",
    srcs = [
        "config.cc",
        "filter.cc",
    ],
    hdrs = [
        "config.h",
        "filter.h",
    ],
    deps = [
        "//envoy/registry",
        "//source/common/common:logger_lib",
        "//source/common/json:json_streamer_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_factory_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_response_lib",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/strings",
        "@envoy_api//envoy/extensions/filters/http/ai_filters/mcp_auth/v3:pkg_cc_proto",
    ],
)
