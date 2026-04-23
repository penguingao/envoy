load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "filter_config_lib",
    hdrs = ["filter_config.h"],
    srcs = ["filter_config.cc"],
    deps = [
        "//envoy/server:filter_config_interface",
        "//envoy/stats:stats_interface",
        "//envoy/stats:stats_macros",
        "//source/common/common:assert_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_factory_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:agentic_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:inference_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:request_decoder_lib",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "filter_lib",
    hdrs = ["filter.h"],
    srcs = ["filter.cc"],
    deps = [
        ":filter_config_lib",
        "//envoy/http:filter_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:logger_lib",
        "//source/common/http:codes_lib",
        "//source/common/http:header_map_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:agentic_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/chain:inference_chain_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_payload_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_response_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:request_decoder_lib",
        "//source/extensions/filters/http/ai_protocol_manager/dispatch:agentic_dispatch_lib",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_extension(
    name = "config",
    hdrs = ["config.h"],
    srcs = ["config.cc"],
    deps = [
        ":filter_config_lib",
        ":filter_lib",
        "//envoy/registry",
        "//source/common/protobuf",
        "//source/extensions/filters/http/ai_protocol_manager/chain:ai_filter_factory_lib",
        "//source/extensions/filters/http/common:factory_base_lib",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
        "@abseil-cpp//absl/strings",
    ],
)
