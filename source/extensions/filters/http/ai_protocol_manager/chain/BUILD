load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "ai_filter_lib",
    hdrs = ["ai_filter.h"],
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
    ],
)

envoy_cc_library(
    name = "ai_filter_callbacks_lib",
    hdrs = ["ai_filter_callbacks.h"],
    deps = [
        ":ai_filter_lib",
        "//envoy/event:dispatcher_interface",
        "//envoy/stream_info:stream_info_interface",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_response_lib",
    ],
)

envoy_cc_library(
    name = "ai_filter_factory_lib",
    hdrs = ["ai_filter_factory.h"],
    deps = [
        ":ai_filter_lib",
        "//envoy/registry",
        "//envoy/server:filter_config_interface",
        "//source/common/protobuf",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "ai_filter_chain_lib",
    hdrs = ["ai_filter_chain.h"],
    srcs = ["ai_filter_chain.cc"],
    deps = [
        ":ai_filter_callbacks_lib",
        ":ai_filter_factory_lib",
        "//source/common/common:assert_lib",
        "//source/common/common:logger_lib",
    ],
)

envoy_cc_library(
    name = "inference_chain_lib",
    hdrs = ["inference_chain.h"],
    deps = [":ai_filter_chain_lib"],
)

envoy_cc_library(
    name = "agentic_chain_lib",
    hdrs = ["agentic_chain.h"],
    deps = [":ai_filter_chain_lib"],
)
