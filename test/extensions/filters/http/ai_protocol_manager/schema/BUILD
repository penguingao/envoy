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
    name = "schema_registry_test",
    srcs = ["schema_registry_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager/schema:schema_registry_lib",
        "//test/test_common:status_utility_lib",
        "//test/test_common:utility_lib",
    ],
)

envoy_extension_cc_test(
    name = "openai_chat_completions_test",
    srcs = ["openai_chat_completions_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        "//envoy/common:exception_lib",
        # The real parser, so the offloaded-string cases meet the DOM shape the
        # filter actually produces rather than a hand-built one.
        "//source/extensions/filters/http/ai_protocol_manager:json_with_ext_buf_parser_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:openai_chat_completions_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:tree_payload_schema_lib",
        "//test/test_common:status_utility_lib",
        "//test/test_common:utility_lib",
    ],
)

envoy_extension_cc_test(
    name = "offload_plan_test",
    srcs = ["offload_plan_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        # The cursor is here so the derived paths can be asserted against the
        # very strings the parser will look them up with.
        "//source/common/json/wuffs_json:wuffs_json_cursor_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:field_schema_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:offload_plan_lib",
        "//test/test_common:status_utility_lib",
        "//test/test_common:utility_lib",
    ],
)

envoy_extension_cc_test(
    name = "schema_validator_test",
    srcs = ["schema_validator_test.cc"],
    extension_names = ["envoy.filters.http.ai_protocol_manager"],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/ai_protocol_manager:json_with_ext_buf_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:field_schema_lib",
        "//source/extensions/filters/http/ai_protocol_manager/schema:schema_validator_lib",
        "//test/test_common:status_utility_lib",
        "//test/test_common:utility_lib",
    ],
)
