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
    name = "sse_event_codec_test",
    srcs = ["sse_event_codec_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    deps = [
        "//source/common/buffer:buffer_lib",
        "//source/common/coroutine:dispatcher_executor_lib",
        "//source/common/coroutine:launch_lib",
        "//source/extensions/filters/http/ai_protocol_manager:chain_writer_lib",
        "//source/extensions/filters/http/ai_protocol_manager:external_buffer_impl_lib",
        "//source/extensions/filters/http/ai_protocol_manager:external_buffer_interface",
        "//source/extensions/filters/http/ai_protocol_manager:ingestion_buffer_manager_lib",
        "//source/extensions/filters/http/ai_protocol_manager:payload_reader_lib",
        "//source/extensions/filters/http/ai_protocol_manager/sse:sse_event_codec_lib",
        "//source/extensions/filters/http/ai_protocol_manager/sse:sse_scanner_lib",
        "//test/extensions/filters/http/ai_protocol_manager:fake_bridge_lib",
        "//test/test_common:status_utility_lib",
        "//test/test_common:utility_lib",
        "@nlohmann_json//:json",
    ],
)
