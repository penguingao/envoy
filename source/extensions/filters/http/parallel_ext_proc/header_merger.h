#pragma once

#include <string>
#include <vector>

#include "envoy/http/header_map.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

// Represents the difference between an original header map and a modified one.
struct HeaderDelta {
  // Headers that were added or had their values changed.
  absl::flat_hash_map<std::string, std::string> set_headers;
  // Headers that were removed.
  absl::flat_hash_set<std::string> removed_headers;
};

class HeaderMerger {
public:
  // Compute the diff between original and modified header maps.
  static HeaderDelta diffHeaders(const Http::HeaderMap& original, const Http::HeaderMap& modified);

  // Merge multiple header deltas by priority and apply to the target header map.
  // Deltas are provided as (priority, delta) pairs. Lower priority number = higher priority
  // (applied last, wins conflicts).
  static void mergeAndApply(const std::vector<std::pair<uint32_t, HeaderDelta>>& deltas,
                            Http::RequestHeaderMap& target);
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
