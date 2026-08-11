#include <string>
#include <vector>

#include "source/common/json/wuffs_json/wuffs_json_cursor.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/offload_plan.h"

#include "test/test_common/status_utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Records the cursor's own pattern path for every string value in a document.
//
// The plan derives its paths from the schema, and the parser will look them up
// with paths the cursor builds mid-parse. That the two agree is the entire
// contract between them, so the tests below assert against paths this produces
// rather than against strings hand-written to match.
class PatternPathRecorder : public Json::Wuffs::WuffsJsonCursor::Handler {
public:
  PatternPathRecorder() : cursor_(*this, /*track_paths=*/true) {}

  absl::Status parse(absl::string_view body) { return cursor_.feed(body, /*closed=*/true); }

  const std::vector<std::string>& stringPaths() const { return string_paths_; }

  bool openStringCapture(absl::string_view, int depth, size_t) override {
    string_paths_.push_back(cursor_.buildPatternPath(depth));
    // Nothing here needs the bytes.
    return false;
  }
  bool onStringChunk(absl::string_view, int, absl::string_view) override { return false; }
  void closeStringCapture(absl::string_view, int, size_t) override {}
  absl::Status onKey(absl::string_view, int, size_t) override { return absl::OkStatus(); }
  absl::Status onNumber(absl::string_view, absl::string_view, int, size_t, size_t) override {
    return absl::OkStatus();
  }
  absl::Status onBoolean(absl::string_view, bool, int, size_t, size_t) override {
    return absl::OkStatus();
  }
  void onNull(absl::string_view, int, size_t, size_t) override {}
  void onContainerOpen(absl::string_view, bool, int, size_t) override {}
  void onContainerClose(int, size_t) override {}

private:
  Json::Wuffs::WuffsJsonCursor cursor_;
  std::vector<std::string> string_paths_;
};

class OffloadPlanTest : public testing::Test {
public:
  // The paths the cursor reports for every string value in `body`, in order.
  std::vector<std::string> cursorPaths(absl::string_view body) {
    PatternPathRecorder recorder;
    EXPECT_OK(recorder.parse(body));
    return recorder.stringPaths();
  }

  SchemaBuilder b_;
};

// The three-way answer, which is the whole point of the class.
TEST_F(OffloadPlanTest, ThreeWayOffloadability) {
  const FieldSchema* schema = b_.object({
      {"prompt", b_.offloadableStr(StreamOrder::Prompt)},
      {"model", b_.str()},
  });
  const OffloadPlan plan(*schema);

  // Declared offloadable.
  EXPECT_TRUE(plan.isOffloadable("prompt"));
  // Declared, and something has to be able to read it.
  EXPECT_FALSE(plan.isOffloadable("model"));
  // Undeclared: pass-through, so nothing constrains it.
  EXPECT_TRUE(plan.isOffloadable("future_field"));
  EXPECT_TRUE(plan.isOffloadable("nested.future_field"));
}

// Only a declared offloadable field has a spec; an undeclared one is offloadable
// without one.
TEST_F(OffloadPlanTest, FindOnlyResolvesDeclaredOffloadableFields) {
  const FieldSchema* schema = b_.object({
      {"prompt", b_.offloadableStr(StreamOrder::Prompt)},
      {"model", b_.str()},
  });
  const OffloadPlan plan(*schema);

  ASSERT_NE(plan.find("prompt"), nullptr);
  EXPECT_EQ(plan.find("prompt")->stream_order, StreamOrder::Prompt);
  EXPECT_EQ(plan.find("model"), nullptr);
  EXPECT_EQ(plan.find("future_field"), nullptr);
}

// The derived paths must be exactly what the cursor builds for the same document.
TEST_F(OffloadPlanTest, DerivedPathsMatchTheCursorsOwnForm) {
  const FieldSchema* part = b_.object({
      {"type", b_.str({"text"})},
      {"text", b_.offloadableStr(StreamOrder::Prompt)},
  });
  const FieldSchema* message = b_.object({
      {"role", b_.str({"user"})},
      {"content", b_.array(part)},
  });
  const FieldSchema* schema = b_.object({
      {"messages", b_.array(message)},
      {"nested", b_.object({{"deep", b_.offloadableStr(StreamOrder::Other)}})},
      {"grid", b_.array(b_.array(b_.offloadableStr(StreamOrder::Other)))},
  });
  const OffloadPlan plan(*schema);

  const std::vector<std::string> paths = cursorPaths(R"({
    "messages":[{"role":"user","content":[{"type":"text","text":"hi"}]}],
    "nested":{"deep":"x"},
    "grid":[["a"]]
  })");
  ASSERT_EQ(paths.size(), 5);

  // Every path the cursor produced is one the plan has an opinion about, and the
  // opinion matches how the schema declared it.
  EXPECT_TRUE(plan.isOffloadable("messages[].content[].text"));
  EXPECT_FALSE(plan.isOffloadable("messages[].role"));
  EXPECT_FALSE(plan.isOffloadable("messages[].content[].type"));
  EXPECT_TRUE(plan.isOffloadable("nested.deep"));
  EXPECT_TRUE(plan.isOffloadable("grid[][]"));

  for (const std::string& path : paths) {
    SCOPED_TRACE(path);
    // Each cursor path is either a declared offloadable field or a declared
    // inline-only one; none of them falls through as undeclared, which is what
    // proves the two path forms agree.
    const bool declared = plan.find(path) != nullptr || !plan.isOffloadable(path);
    EXPECT_TRUE(declared);
  }
}

// A string directly inside an array gets the array's own path.
TEST_F(OffloadPlanTest, StringInsideAnArray) {
  const FieldSchema* schema = b_.object({{"stop", b_.array(b_.str())}});
  const OffloadPlan plan(*schema);

  EXPECT_EQ(cursorPaths(R"({"stop":["a","b"]})"), (std::vector<std::string>{"stop[]", "stop[]"}));
  EXPECT_FALSE(plan.isOffloadable("stop[]"));
}

// Both branches of a oneOf sit at the same path, which is how the string form and
// the parts-array form of a content field are both described.
TEST_F(OffloadPlanTest, OneOfDescribesEveryAlternativeAtTheSamePath) {
  const FieldSchema* part = b_.object({{"text", b_.offloadableStr(StreamOrder::Prompt)}});
  const FieldSchema* schema = b_.object({
      {"content", b_.oneOf({b_.offloadableStr(StreamOrder::Prompt), b_.array(part)})},
  });
  const OffloadPlan plan(*schema);

  EXPECT_TRUE(plan.isOffloadable("content"));
  EXPECT_TRUE(plan.isOffloadable("content[].text"));
  EXPECT_NE(plan.find("content"), nullptr);
  EXPECT_NE(plan.find("content[].text"), nullptr);
}

// A field something has to read wins over one that may be offloaded, so a path
// declared both ways stays inline.
TEST_F(OffloadPlanTest, InlineOnlyWinsOverOffloadable) {
  const FieldSchema* schema = b_.object({
      {"f", b_.oneOf({b_.offloadableStr(StreamOrder::Prompt), b_.str({"sentinel"})})},
  });
  const OffloadPlan plan(*schema);

  EXPECT_FALSE(plan.isOffloadable("f"));
  EXPECT_EQ(plan.find("f"), nullptr);
}

// An AnyJson subtree declares nothing, so strings inside a caller's tool schema
// are undeclared and therefore offloadable -- which is what keeps a large tool
// description out of the DOM.
TEST_F(OffloadPlanTest, AnyJsonSubtreeDeclaresNothing) {
  const FieldSchema* schema = b_.object({{"parameters", b_.anyJson()}});
  const OffloadPlan plan(*schema);

  EXPECT_TRUE(plan.isOffloadable("parameters"));
  EXPECT_TRUE(plan.isOffloadable("parameters.properties.city.description"));
  EXPECT_TRUE(plan.streamOrder().empty());
}

// Non-string kinds contribute nothing: a number is never offloaded, so its path
// would change no decision.
TEST_F(OffloadPlanTest, NonStringKindsAreNotRecorded) {
  const FieldSchema* schema = b_.object({
      {"temperature", b_.number(0, 2)},
      {"n", b_.integer(1)},
      {"stream", b_.boolean()},
  });
  const OffloadPlan plan(*schema);

  EXPECT_TRUE(plan.streamOrder().empty());
  // Not recorded as inline-only either, so they read as undeclared.
  EXPECT_TRUE(plan.isOffloadable("temperature"));
}

// Prompts stream before tools, and the order does not depend on hash iteration.
TEST_F(OffloadPlanTest, StreamOrderIsSortedAndDeterministic) {
  const FieldSchema* schema = b_.object({
      {"zzz_tool", b_.offloadableStr(StreamOrder::Tool)},
      {"aaa_other", b_.offloadableStr(StreamOrder::Other)},
      {"mmm_prompt", b_.offloadableStr(StreamOrder::Prompt)},
      {"bbb_prompt", b_.offloadableStr(StreamOrder::Prompt)},
  });
  const OffloadPlan plan(*schema);

  std::vector<std::string> ordered;
  for (const OffloadSpec& spec : plan.streamOrder()) {
    ordered.push_back(spec.pattern_path);
  }
  // Prompts first, ties broken on path; then tools; then the rest.
  EXPECT_EQ(ordered,
            (std::vector<std::string>{"bbb_prompt", "mmm_prompt", "zzz_tool", "aaa_other"}));

  // Rebuilding gives the same order.
  const OffloadPlan again(*schema);
  std::vector<std::string> ordered_again;
  for (const OffloadSpec& spec : again.streamOrder()) {
    ordered_again.push_back(spec.pattern_path);
  }
  EXPECT_EQ(ordered, ordered_again);
}

// A node shared by two parents describes two distinct paths, which is what lets
// one content-part schema serve two fields.
TEST_F(OffloadPlanTest, SharedNodeYieldsOnePathPerParent) {
  const FieldSchema* part = b_.object({{"text", b_.offloadableStr(StreamOrder::Prompt)}});
  const FieldSchema* schema = b_.object({
      {"messages", b_.array(b_.object({{"content", b_.array(part)}}))},
      {"prediction", b_.object({{"content", b_.array(part)}})},
  });
  const OffloadPlan plan(*schema);

  EXPECT_TRUE(plan.isOffloadable("messages[].content[].text"));
  EXPECT_TRUE(plan.isOffloadable("prediction.content[].text"));
  EXPECT_EQ(plan.streamOrder().size(), 2);
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
