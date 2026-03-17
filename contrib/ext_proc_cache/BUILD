load("//bazel:envoy_build_system.bzl", "envoy_cc_binary", "envoy_cc_library", "envoy_contrib_package")

licenses(["notice"])  # Apache 2

envoy_contrib_package()

envoy_cc_library(
    name = "server_lib",
    srcs = ["server.cc"],
    hdrs = ["server.h"],
    deps = [
        "@com_github_grpc_grpc//:grpc++",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_grpc",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_proto",
    ],
)

envoy_cc_binary(
    name = "ext_proc_cache_server",
    srcs = ["main.cc"],
    deps = [
        ":server_lib",
    ],
)
