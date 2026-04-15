load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "capture_filter_lib",
    hdrs = ["capture_filter.h"],
    deps = [
        "//envoy/http:filter_interface",
    ],
)

envoy_cc_library(
    name = "header_merger_lib",
    srcs = ["header_merger.cc"],
    hdrs = ["header_merger.h"],
    deps = [
        "//envoy/http:header_map_interface",
        "//source/common/http:header_utility_lib",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
    ],
)

envoy_cc_library(
    name = "processor_chain_lib",
    srcs = ["processor_chain.cc"],
    hdrs = ["processor_chain.h"],
    deps = [
        ":capture_filter_lib",
        "//envoy/http:filter_interface",
        "//envoy/network:connection_interface",
        "//source/common/common:logger_lib",
        "//source/common/http:filter_manager_lib",
        "//source/common/http:header_map_lib",
    ],
)

envoy_cc_library(
    name = "parallel_ext_proc_lib",
    srcs = ["parallel_filter.cc"],
    hdrs = ["parallel_filter.h"],
    deps = [
        ":header_merger_lib",
        ":processor_chain_lib",
        "//envoy/event:timer_interface",
        "//envoy/http:filter_interface",
        "//source/common/common:logger_lib",
        "//source/common/http:header_map_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "@envoy_api//envoy/extensions/filters/http/parallel_ext_proc/v3:pkg_cc_proto",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":parallel_ext_proc_lib",
        "//source/extensions/filters/http/common:factory_base_lib",
        "//source/extensions/filters/http/ext_proc:config",
        "@envoy_api//envoy/extensions/filters/http/parallel_ext_proc/v3:pkg_cc_proto",
    ],
)
