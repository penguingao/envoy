#include <chrono>
#include <memory>
#include <vector>

#include "envoy/extensions/filters/http/parallel_ext_proc/v3/parallel_ext_proc.pb.h"

#include "source/extensions/filters/http/parallel_ext_proc/parallel_filter.h"

#include "test/mocks/http/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {
namespace {

using ::testing::NiceMock;

// Verifies the aggregation logic that combines watermark signals from every
// sub-chain into a single backpressure signal on the main decoder_callbacks_.
// The signal should fire exactly once when the first sub-chain crosses the
// high watermark, and fire below exactly once when the last sub-chain drops
// below the low watermark.
class ParallelFilterWatermarkTest : public testing::Test {
protected:
  void initialize(uint32_t num_chains) {
    std::vector<ProcessorInfo> processors;
    processors.reserve(num_chains);
    for (uint32_t i = 0; i < num_chains; ++i) {
      ProcessorInfo info;
      info.name = absl::StrCat("processor_", i);
      info.priority = i;
      info.ext_proc_factory_cb = [](Http::FilterChainFactoryCallbacks&) {};
      info.can_modify_body = false;
      processors.push_back(std::move(info));
    }

    envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy failure_policy;
    config_ = std::make_shared<ParallelFilterConfig>(std::move(processors), failure_policy,
                                                     std::chrono::milliseconds(0));
    filter_ = std::make_unique<ParallelExtProcFilter>(config_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
  }

  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  ParallelFilterConfigSharedPtr config_;
  std::unique_ptr<ParallelExtProcFilter> filter_;
};

// Single chain crossing the high watermark and returning below it produces a
// matched pair of high/low signals on the main decoder_callbacks_.
TEST_F(ParallelFilterWatermarkTest, SingleChainTogglesBackpressure) {
  initialize(/*num_chains=*/1);

  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark()).Times(1);
  filter_->onChainAboveHighWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark()).Times(1);
  filter_->onChainBelowLowWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
}

// When multiple chains are above the high watermark, the backpressure signal
// is sent exactly once (on the first chain crossing up) and cleared exactly
// once (after every chain has dropped below the low watermark).
TEST_F(ParallelFilterWatermarkTest, AggregatesAcrossChains) {
  initialize(/*num_chains=*/3);

  // First chain crosses up: should fire once.
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark()).Times(1);
  filter_->onChainAboveHighWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

  // Second and third chains also cross up: no additional fires.
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark()).Times(0);
  filter_->onChainAboveHighWatermark();
  filter_->onChainAboveHighWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

  // First chain drops below low: still under backpressure (2 of 3 remain).
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark()).Times(0);
  filter_->onChainBelowLowWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

  // Second chain below: still 1 remaining.
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark()).Times(0);
  filter_->onChainBelowLowWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);

  // Last chain below: fire below signal exactly once.
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark()).Times(1);
  filter_->onChainBelowLowWatermark();
  testing::Mock::VerifyAndClearExpectations(&decoder_callbacks_);
}

// Rapid toggling of a single chain produces a matched sequence of high/low
// signals without dropping any transition.
TEST_F(ParallelFilterWatermarkTest, RapidToggle) {
  initialize(/*num_chains=*/1);

  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark()).Times(3);
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark()).Times(3);

  filter_->onChainAboveHighWatermark();
  filter_->onChainBelowLowWatermark();
  filter_->onChainAboveHighWatermark();
  filter_->onChainBelowLowWatermark();
  filter_->onChainAboveHighWatermark();
  filter_->onChainBelowLowWatermark();
}

} // namespace
} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
