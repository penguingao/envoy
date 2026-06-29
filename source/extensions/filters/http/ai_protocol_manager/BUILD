load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "ai_protocol_manager_buffer_lib",
    srcs = ["in_memory_buffer.cc"],
    hdrs = [
        "external_buffer.h",
        "in_memory_buffer.h",
    ],
    deps = [
        "//envoy/buffer:buffer_interface",
        "//source/common/buffer:buffer_lib",
    ],
)

envoy_cc_library(
    name = "ai_protocol_manager_lib",
    srcs = [
        "ai_protocol_manager.cc",
        "buffer_manager.cc",
    ],
    hdrs = [
        "ai_protocol_manager.h",
        "buffer_manager.h",
    ],
    deps = [
        ":ai_protocol_manager_buffer_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":ai_protocol_manager_lib",
        "//envoy/registry",
        "//source/extensions/filters/http/common:factory_base_lib",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
    ],
)
