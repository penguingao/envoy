load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "auto_router_lib",
    srcs = ["filter.cc"],
    hdrs = ["filter.h"],
    deps = [
        "//source/common/common:logger_lib",
        "//source/common/common:matchers_lib",
        "//source/common/coroutine:status_macros_lib",
        "//source/extensions/filters/ai:ai_filter_interface",
        "@abseil-cpp//absl/strings",
        "@envoy_api//envoy/extensions/filters/ai/auto_router/v3:pkg_cc_proto",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":auto_router_lib",
        "//envoy/registry",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/filters/ai:ai_filter_interface",
        "@envoy_api//envoy/extensions/filters/ai/auto_router/v3:pkg_cc_proto",
    ],
)
