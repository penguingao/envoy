load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "anthropic_inference_integration_test",
    srcs = ["anthropic_inference_integration_test.cc"],
    deps = [
        # Pull in the AiProtocolManager config registration so the factory is
        # available at runtime.  This transitively brings in all dispatch and
        # provider encoder code (inference_dispatch_lib, anthropic_request_encoder_lib).
        "//source/extensions/filters/http/ai_protocol_manager:config",
        "//test/integration:http_integration_lib",
        "@envoy_api//envoy/extensions/filters/http/ai_protocol_manager/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/filters/network/http_connection_manager/v3:pkg_cc_proto",
    ],
)

envoy_cc_test(
    name = "payload_store_test",
    srcs = ["payload_store_test.cc"],
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/codec:mmap_payload_store_lib",
        "//source/common/buffer:buffer_lib",
    ],
)

envoy_cc_test(
    name = "request_decoder_test",
    srcs = ["request_decoder_test.cc"],
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/codec:request_decoder_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_payload_lib",
        "//source/extensions/filters/http/ai_protocol_manager/codec:ai_request_lib",
        "//test/test_common:utility_lib",
    ],
)
