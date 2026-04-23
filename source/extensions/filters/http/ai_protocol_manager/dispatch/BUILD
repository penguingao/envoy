load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "agentic_dispatch_lib",
    hdrs = ["agentic_dispatch.h"],
    srcs = ["agentic_dispatch.cc"],
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:request_encoder_lib",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:assert_lib",
        "//source/common/common:logger_lib",
        "@abseil-cpp//absl/strings",
    ],
)
