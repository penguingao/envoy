#include "source/extensions/filters/http/parallel_ext_proc/header_merger.h"

#include <algorithm>

#include "source/common/http/header_utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

HeaderDelta HeaderMerger::diffHeaders(const Http::HeaderMap& original,
                                      const Http::HeaderMap& modified) {
  HeaderDelta delta;

  // Find headers that were added or changed in the modified map.
  modified.iterate([&original, &delta](const Http::HeaderEntry& entry) -> Http::HeaderMap::Iterate {
    const auto key = entry.key().getStringView();
    const auto value = entry.value().getStringView();
    const auto original_result = original.get(Http::LowerCaseString(std::string(key)));
    if (original_result.empty() || original_result[0]->value().getStringView() != value) {
      delta.set_headers[std::string(key)] = std::string(value);
    }
    return Http::HeaderMap::Iterate::Continue;
  });

  // Find headers that were removed (present in original but not in modified).
  original.iterate([&modified, &delta](const Http::HeaderEntry& entry) -> Http::HeaderMap::Iterate {
    const auto key = entry.key().getStringView();
    const auto modified_result = modified.get(Http::LowerCaseString(std::string(key)));
    if (modified_result.empty()) {
      delta.removed_headers.insert(std::string(key));
    }
    return Http::HeaderMap::Iterate::Continue;
  });

  return delta;
}

void HeaderMerger::mergeAndApply(const std::vector<std::pair<uint32_t, HeaderDelta>>& deltas,
                                 Http::RequestHeaderMap& target) {
  if (deltas.empty()) {
    return;
  }

  // Sort by priority descending (lowest priority first, so highest priority applied last = wins).
  std::vector<std::pair<uint32_t, const HeaderDelta*>> sorted;
  sorted.reserve(deltas.size());
  for (const auto& [priority, delta] : deltas) {
    sorted.emplace_back(priority, &delta);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  // Accumulate mutations. Later (higher priority) entries overwrite earlier ones.
  absl::flat_hash_map<std::string, std::string> merged_set;
  absl::flat_hash_set<std::string> merged_remove;

  for (const auto& [priority, delta_ptr] : sorted) {
    for (const auto& key : delta_ptr->removed_headers) {
      merged_remove.insert(key);
      merged_set.erase(key);
    }
    for (const auto& [key, value] : delta_ptr->set_headers) {
      merged_set[key] = value;
      merged_remove.erase(key);
    }
  }

  // Apply removals first.
  for (const auto& key : merged_remove) {
    target.remove(Http::LowerCaseString(key));
  }

  // Apply sets/additions.
  for (const auto& [key, value] : merged_set) {
    const Http::LowerCaseString lower_key(key);
    target.remove(lower_key);
    target.addCopy(lower_key, value);
  }
}

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
