load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "parallel_ext_proc_integration_test",
    size = "large",
    srcs = ["parallel_ext_proc_integration_test.cc"],
    extension_names = [
        "envoy.filters.http.parallel_ext_proc",
        "envoy.filters.http.ext_proc",
    ],
    rbe_pool = "4core",
    shard_count = 4,
    tags = [
        "cpu:3",
        "skip_on_windows",
    ],
    deps = [
        "//source/common/protobuf:utility_lib",
        "//source/extensions/filters/http/ext_proc:config",
        "//source/extensions/filters/http/parallel_ext_proc:config",
        "//test/common/grpc:grpc_client_integration_lib",
        "//test/common/http:common_lib",
        "//test/integration:http_integration_lib",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/ext_proc/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/http/parallel_ext_proc/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/network/http_connection_manager/v3:pkg_cc_proto",
        "@envoy_api//envoy/service/ext_proc/v3:pkg_cc_proto",
    ],
)
