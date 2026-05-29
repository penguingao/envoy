load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_library(
    name = "wuffs_json_lib",
    srcs = [
        "wuffs_impl.c",
        "wuffs_json.cc",
    ],
    hdrs = ["wuffs_json.h"],
    copts = ["-Wno-unused-function"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@wuffs//:wuffs",
    ],
)
