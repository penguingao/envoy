load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

# Nothing in this package may depend on a filter target (filter_lib,
# buffer_manager_lib, filter_chain_bridge_lib): a schema describes a payload and
# knows nothing about the stream carrying it. The arrow points one way only, and
# Bazel is what keeps it that way.

envoy_cc_library(
    name = "field_schema_lib",
    srcs = ["field_schema.cc"],
    hdrs = ["field_schema.h"],
    deps = [
        "//source/common/common:assert_lib",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "schema_validator_lib",
    srcs = ["schema_validator.cc"],
    hdrs = ["schema_validator.h"],
    deps = [
        ":field_schema_lib",
        "//envoy/common:exception_lib",
        "//source/extensions/filters/http/ai_protocol_manager:json_with_ext_buf_lib",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@nlohmann_json//:json",
    ],
)

envoy_cc_library(
    name = "payload_schema_interface",
    hdrs = ["payload_schema.h"],
    deps = [
        "//envoy/common:pure_lib",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@nlohmann_json//:json",
    ],
)

envoy_cc_library(
    name = "schema_registry_lib",
    srcs = ["schema_registry.cc"],
    hdrs = ["schema_registry.h"],
    deps = [
        ":openai_chat_completions_lib",
        ":payload_schema_interface",
        ":tree_payload_schema_lib",
        "//source/common/common:assert_lib",
        "//source/common/singleton:const_singleton",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "openai_chat_completions_lib",
    srcs = ["openai_chat_completions.cc"],
    hdrs = ["openai_chat_completions.h"],
    deps = [":field_schema_lib"],
)

envoy_cc_library(
    name = "tree_payload_schema_lib",
    srcs = ["tree_payload_schema.cc"],
    hdrs = ["tree_payload_schema.h"],
    deps = [
        ":field_schema_lib",
        ":payload_schema_interface",
        ":schema_validator_lib",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@nlohmann_json//:json",
    ],
)
