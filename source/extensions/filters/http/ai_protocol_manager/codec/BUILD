load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "ai_payload_lib",
    hdrs = ["ai_payload.h"],
    srcs = ["ai_payload.cc"],
    deps = [
        "//envoy/buffer:buffer_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:assert_lib",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "ai_request_lib",
    hdrs = ["ai_request.h"],
    srcs = ["ai_request.cc"],
    deps = [
        ":ai_payload_lib",
        "//envoy/event:dispatcher_interface",
        "//envoy/http:header_map_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:assert_lib",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/types:optional",
    ],
)

envoy_cc_library(
    name = "ai_response_lib",
    hdrs = ["ai_response.h"],
)

envoy_cc_library(
    name = "mmap_payload_store_lib",
    hdrs = ["mmap_payload_store.h"],
    srcs = ["mmap_payload_store.cc"],
    deps = [
        ":ai_payload_lib",
        "//envoy/event:dispatcher_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:assert_lib",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "protocol_classifier_lib",
    hdrs = ["protocol_classifier.h"],
    srcs = ["protocol_classifier.cc"],
    deps = [
        ":ai_request_lib",
        "//envoy/http:header_map_interface",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "wuffs_json_lib",
    hdrs = ["wuffs_json.h"],
    srcs = [
        "wuffs_json.cc",
        "wuffs_impl.c",
    ],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@wuffs//:wuffs",
    ],
    copts = ["-Wno-unused-function"],
)

envoy_cc_library(
    name = "request_decoder_lib",
    hdrs = ["request_decoder.h"],
    srcs = ["request_decoder.cc"],
    deps = [
        ":ai_request_lib",
        ":protocol_classifier_lib",
        ":wuffs_json_lib",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:assert_lib",
        "//source/common/common:logger_lib",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@wuffs//:wuffs",
    ],
    copts = ["-Wno-unused-function"],
)

envoy_cc_library(
    name = "request_encoder_lib",
    hdrs = ["request_encoder.h"],
    srcs = ["request_encoder.cc"],
    deps = [
        ":ai_request_lib",
        "//source/common/common:assert_lib",
        "//source/common/common:regex_lib",
        "//source/common/http:utility_lib",
        "//source/common/json:json_streamer_lib",
        "//source/extensions/filters/http/ai_protocol_manager:rest_transcoder_config_lib",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:optional",
        "@abseil-cpp//absl/types:span",
        "@nlohmann_json//:json",
    ],
)
