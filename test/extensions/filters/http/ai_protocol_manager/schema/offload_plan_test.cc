#include <algorithm>
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
      {"prompt", b_.offloadableStr()},
      {"model", b_.str()},
  });
  const OffloadPlan plan(*schema, {});

  // Declared offloadable.
  EXPECT_TRUE(plan.isOffloadable("prompt"));
  // Declared, and something has to be able to read it.
  EXPECT_FALSE(plan.isOffloadable("model"));
  // Undeclared: pass-through, so nothing constrains it.
  EXPECT_TRUE(plan.isOffloadable("future_field"));
  EXPECT_TRUE(plan.isOffloadable("nested.future_field"));
}

// Only declared offloadable fields are in the streaming order; an undeclared field
// is offloadable but is not something the chain waits on.
TEST_F(OffloadPlanTest, StreamOrderHoldsOnlyDeclaredOffloadableFields) {
  const FieldSchema* schema = b_.object({
      {"prompt", b_.offloadableStr()},
      {"model", b_.str()},
  });
  const OffloadPlan plan(*schema, {});

  EXPECT_EQ(plan.streamOrder(), (std::vector<std::string>{"prompt"}));
}

// The derived paths must be exactly what the cursor builds for the same document.
TEST_F(OffloadPlanTest, DerivedPathsMatchTheCursorsOwnForm) {
  const FieldSchema* part = b_.object({
      {"type", b_.str()},
      {"text", b_.offloadableStr()},
  });
  const FieldSchema* message = b_.object({
      {"role", b_.str()},
      {"content", b_.array(part)},
  });
  const FieldSchema* schema = b_.object({
      {"messages", b_.array(message)},
      {"nested", b_.object({{"deep", b_.offloadableStr()}})},
      {"grid", b_.array(b_.array(b_.offloadableStr()))},
  });
  const OffloadPlan plan(*schema, {});

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
    // Each cursor path is one the schema declared -- either offloadable (so it is
    // in the streaming order) or inline-only. None falls through as undeclared,
    // which is what proves the two path forms agree.
    const auto& order = plan.streamOrder();
    const bool declared =
        std::find(order.begin(), order.end(), path) != order.end() || !plan.isOffloadable(path);
    EXPECT_TRUE(declared);
  }
}

// A string directly inside an array gets the array's own path.
TEST_F(OffloadPlanTest, StringInsideAnArray) {
  const FieldSchema* schema = b_.object({{"stop", b_.array(b_.str())}});
  const OffloadPlan plan(*schema, {});

  EXPECT_EQ(cursorPaths(R"({"stop":["a","b"]})"), (std::vector<std::string>{"stop[]", "stop[]"}));
  EXPECT_FALSE(plan.isOffloadable("stop[]"));
}

// Both branches of a oneOf sit at the same path, which is how the string form and
// the parts-array form of a content field are both described.
TEST_F(OffloadPlanTest, OneOfDescribesEveryAlternativeAtTheSamePath) {
  const FieldSchema* part = b_.object({{"text", b_.offloadableStr()}});
  const FieldSchema* schema = b_.object({
      {"content", b_.oneOf({b_.offloadableStr(), b_.array(part)})},
  });
  const OffloadPlan plan(*schema, {});

  EXPECT_TRUE(plan.isOffloadable("content"));
  EXPECT_TRUE(plan.isOffloadable("content[].text"));
  EXPECT_EQ(plan.streamOrder(), (std::vector<std::string>{"content", "content[].text"}));
}

// A field something has to read wins over one that may be offloaded, so a path
// declared both ways stays inline.
TEST_F(OffloadPlanTest, InlineOnlyWinsOverOffloadable) {
  const FieldSchema* schema = b_.object({
      {"f", b_.oneOf({b_.offloadableStr(), b_.str()})},
  });
  const OffloadPlan plan(*schema, {});

  EXPECT_FALSE(plan.isOffloadable("f"));
  EXPECT_TRUE(plan.streamOrder().empty());
}

// An AnyJson subtree declares nothing, so strings inside a caller's tool schema
// are undeclared and therefore offloadable -- which is what keeps a large tool
// description out of the DOM.
TEST_F(OffloadPlanTest, AnyJsonSubtreeDeclaresNothing) {
  const FieldSchema* schema = b_.object({{"parameters", b_.anyJson()}});
  const OffloadPlan plan(*schema, {});

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
  const OffloadPlan plan(*schema, {});

  EXPECT_TRUE(plan.streamOrder().empty());
  // Not recorded as inline-only either, so they read as undeclared.
  EXPECT_TRUE(plan.isOffloadable("temperature"));
}

// The declared order is the order, whatever the field names happen to sort as.
TEST_F(OffloadPlanTest, StreamOrderFollowsTheDeclaredList) {
  const FieldSchema* schema = b_.object({
      {"aaa", b_.offloadableStr()},
      {"mmm", b_.offloadableStr()},
      {"zzz", b_.offloadableStr()},
  });
  const std::vector<absl::string_view> declared = {"zzz", "aaa", "mmm"};
  const OffloadPlan plan(*schema, declared);

  EXPECT_EQ(plan.streamOrder(), (std::vector<std::string>{"zzz", "aaa", "mmm"}));
}

// Offloadable fields the list omits go after the ones it names, sorted so the
// result does not depend on hash iteration.
TEST_F(OffloadPlanTest, UnlistedFieldsGoLastInADeterministicOrder) {
  const FieldSchema* schema = b_.object({
      {"zzz_listed", b_.offloadableStr()},
      {"bbb_unlisted", b_.offloadableStr()},
      {"aaa_unlisted", b_.offloadableStr()},
  });
  const std::vector<absl::string_view> declared = {"zzz_listed"};
  const OffloadPlan plan(*schema, declared);

  EXPECT_EQ(plan.streamOrder(),
            (std::vector<std::string>{"zzz_listed", "aaa_unlisted", "bbb_unlisted"}));

  // Rebuilding gives the same order.
  const OffloadPlan again(*schema, declared);
  EXPECT_EQ(plan.streamOrder(), again.streamOrder());
}

// A path in the order that is not a declared offloadable field is a typo, and a
// typo would otherwise silently sink the field to the end.
TEST_F(OffloadPlanTest, StreamOrderRejectsAPathThatIsNotOffloadable) {
  const FieldSchema* schema = b_.object({
      {"prompt", b_.offloadableStr()},
      {"model", b_.str()},
  });

  // Misspelled.
  EXPECT_DEATH(OffloadPlan(*schema, std::vector<absl::string_view>{"promt"}), "stream order");
  // Declared, but inline-only.
  EXPECT_DEATH(OffloadPlan(*schema, std::vector<absl::string_view>{"model"}), "stream order");
  // Not declared at all.
  EXPECT_DEATH(OffloadPlan(*schema, std::vector<absl::string_view>{"future_field"}),
               "stream order");
}

// A node shared by two parents describes two distinct paths, which is what lets
// one content-part schema serve two fields.
TEST_F(OffloadPlanTest, SharedNodeYieldsOnePathPerParent) {
  const FieldSchema* part = b_.object({{"text", b_.offloadableStr()}});
  const FieldSchema* schema = b_.object({
      {"messages", b_.array(b_.object({{"content", b_.array(part)}}))},
      {"prediction", b_.object({{"content", b_.array(part)}})},
  });
  const OffloadPlan plan(*schema, {});

  EXPECT_TRUE(plan.isOffloadable("messages[].content[].text"));
  EXPECT_TRUE(plan.isOffloadable("prediction.content[].text"));
  EXPECT_EQ(plan.streamOrder().size(), 2);
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
