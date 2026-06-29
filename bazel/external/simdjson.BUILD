load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "simdjson",
    srcs = ["simdjson.cpp"],
    hdrs = ["simdjson.h"],
    # simdjson uses C-style casts inside ARM NEON intrinsic macros (arm_neon.h).
    # Suppress the warning so this third-party target builds under Envoy's
    # -Werror,-Wold-style-cast policy.
    copts = ["-Wno-old-style-cast"],
)
