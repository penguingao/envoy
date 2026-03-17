load("//bazel:envoy_build_system.bzl", "envoy_cc_binary", "envoy_cc_library", "envoy_cc_test", "envoy_contrib_package")

licenses(["notice"])  # Apache 2

envoy_contrib_package()

envoy_cc_library(
    name = "awaitable_lib",
    hdrs = ["awaitable.h"],
)

envoy_cc_library(
    name = "cache_types_lib",
    srcs = ["cache_types.cc"],
    hdrs = ["cache_types.h"],
    deps = [
        "@abseil-cpp//absl/strings",
        "@envoy_api//envoy/config/core/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "cache_store_lib",
    hdrs = ["cache_store.h"],
    deps = [
        ":awaitable_lib",
        ":cache_types_lib",
    ],
)

envoy_cc_library(
    name = "in_memory_cache_store_lib",
    srcs = ["in_memory_cache_store.cc"],
    hdrs = ["in_memory_cache_store.h"],
    deps = [
        ":cache_store_lib",
    ],
)

envoy_cc_library(
    name = "cache_key_generator_lib",
    hdrs = ["cache_key_generator.h"],
    deps = [
        ":cache_types_lib",
        "@abseil-cpp//absl/strings",
    ],
)

envoy_cc_library(
    name = "cacheability_checker_lib",
    hdrs = ["cacheability_checker.h"],
    deps = [
        ":cache_types_lib",
        "@abseil-cpp//absl/container:flat_hash_set",
    ],
)

envoy_cc_library(
    name = "cache_age_calculator_lib",
    hdrs = ["cache_age_calculator.h"],
    deps = [
        ":cache_types_lib",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
    ],
)

envoy_cc_library(
    name = "cache_lookup_coordinator_lib",
    srcs = ["cache_lookup_coordinator.cc"],
    hdrs = ["cache_lookup_coordinator.h"],
    deps = [
        ":awaitable_lib",
        ":cache_store_lib",
        ":cache_types_lib",
    ],
)

envoy_cc_library(
    name = "cache_stream_handler_lib",
    srcs = ["cache_stream_handler.cc"],
    hdrs = ["cache_stream_handler.h"],
    deps = [
        ":awaitable_lib",
        ":cache_age_calculator_lib",
        ":cache_key_generator_lib",
        ":cache_lookup_coordinator_lib",
        ":cache_types_lib",
        ":cacheability_checker_lib",
        "@abseil-cpp//absl/strings",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "server_lib",
    srcs = ["server.cc"],
    hdrs = ["server.h"],
    deps = [
        ":cache_age_calculator_lib",
        ":cache_key_generator_lib",
        ":cache_lookup_coordinator_lib",
        ":cache_stream_handler_lib",
        ":cacheability_checker_lib",
        "@com_github_grpc_grpc//:grpc++",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_grpc",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_proto",
    ],
)

envoy_cc_binary(
    name = "ext_proc_cache_server",
    srcs = ["main.cc"],
    deps = [
        ":cache_age_calculator_lib",
        ":cache_key_generator_lib",
        ":cache_lookup_coordinator_lib",
        ":cacheability_checker_lib",
        ":in_memory_cache_store_lib",
        ":server_lib",
    ],
)

envoy_cc_test(
    name = "ext_proc_cache_integration_test",
    size = "large",
    srcs = ["ext_proc_cache_integration_test.cc"],
    deps = [
        ":cache_age_calculator_lib",
        ":cache_key_generator_lib",
        ":cache_lookup_coordinator_lib",
        ":cacheability_checker_lib",
        ":in_memory_cache_store_lib",
        ":server_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/filters/http/ext_proc:config",
        "//test/integration:http_integration_lib",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/ext_proc/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/network/http_connection_manager/v3:pkg_cc_proto",
    ],
)
