load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "anthropic_request_encoder_lib",
    hdrs = ["anthropic_request_encoder.h"],
    srcs = ["anthropic_request_encoder.cc"],
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:request_encoder_lib",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:optional",
        "@nlohmann_json//:json",
    ],
)
