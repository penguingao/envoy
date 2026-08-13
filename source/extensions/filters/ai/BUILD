load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "ai_filter_interface",
    hdrs = ["ai_filter.h"],
    deps = [
        "//envoy/common:pure_lib",
        "//envoy/config:typed_config_interface",
        "//envoy/event:dispatcher_interface",
        "//envoy/http:codes_interface",
        "//envoy/http:header_map_interface",
        "//envoy/server:factory_context_interface",
        "//envoy/stream_info:stream_info_interface",
        "//source/common/coroutine:task_lib",
        "//source/extensions/filters/http/ai_protocol_manager:inference_request_lib",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
