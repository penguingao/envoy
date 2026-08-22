#include <memory>
#include <string>

#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/normalization_filter.h"

#include "test/test_common/status_utility.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

using ::Envoy::StatusHelpers::HasStatusCode;

class NormalizationFilterTest : public testing::Test {
public:
  NormalizationFilterTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test")) {}

  void drain() { dispatcher_->run(Event::Dispatcher::RunType::NonBlock); }

  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
};

TEST_F(NormalizationFilterTest, AddsMessagesArrayWhenMissing) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<NormalizationFilter>());

  FilterManager manager(std::move(filters), std::move(doc), nullptr, *dispatcher_);

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();

  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  const auto& parsed = manager.requestJson().json();
  EXPECT_EQ(parsed["model"], "gpt-4");
  ASSERT_TRUE(parsed.contains("messages"));
  EXPECT_TRUE(parsed["messages"].is_array());
  EXPECT_TRUE(parsed["messages"].empty());
}

TEST_F(NormalizationFilterTest, PreservesExistingMessages) {
  JsonWithExtBuf doc;
  doc.setJson(nlohmann::json{
      {"model", "gpt-4"},
      {"messages", nlohmann::json::array({
                       {{"role", "user"}, {"content", "hello"}},
                   })},
  });

  std::vector<AiFilterPtr> filters;
  filters.push_back(std::make_unique<NormalizationFilter>());

  FilterManager manager(std::move(filters), std::move(doc), nullptr, *dispatcher_);

  absl::Status status;
  bool completed = false;

  manager.start([&status, &completed](absl::Status s) {
    status = std::move(s);
    completed = true;
  });

  drain();

  EXPECT_TRUE(completed);
  ASSERT_OK(status);

  const auto& parsed = manager.requestJson().json();
  EXPECT_EQ(parsed["model"], "gpt-4");
  ASSERT_EQ(parsed["messages"].size(), 1);
  EXPECT_EQ(parsed["messages"][0]["role"], "user");
  EXPECT_EQ(parsed["messages"][0]["content"], "hello");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
