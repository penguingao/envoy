#pragma once

#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "contrib/ext_proc_cache/awaitable.h"
#include "contrib/ext_proc_cache/cache_store.h"
#include "contrib/ext_proc_cache/cache_types.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Shared coalescing layer that prevents thundering herd by funneling concurrent
// lookups for the same key through a single cache fill.
class CacheLookupCoordinator {
public:
  explicit CacheLookupCoordinator(std::shared_ptr<CacheStore> store,
                                  size_t max_fill_retries = 3);

  // Called by per-stream handler. Suspends if another stream is already filling.
  Awaitable<CoordinatedLookupResult> lookup(const std::string& key,
                                            std::chrono::system_clock::time_point deadline);

  // Called by the filler stream after successful store to CacheStore.
  void reportFillSuccess(const std::string& key, const CachedEntry& entry);

  // Called by the filler stream when upstream/store fails.
  void reportFillFailure(const std::string& key);

  // Called when a waiting stream is cancelled (from OnCancel).
  void cancelWaiter(const std::string& key, std::coroutine_handle<> handle);

  // Access the store directly (e.g., for filler to store entries).
  std::shared_ptr<CacheStore> store() const { return store_; }

private:
  struct Waiter {
    std::coroutine_handle<> handle;
    std::chrono::system_clock::time_point deadline;
    CoordinatedLookupResult* result_ptr;
  };

  struct PendingKey {
    std::vector<Waiter> waiters;
    size_t fill_attempts = 0;
    bool filling = false;
  };

  // Pick waiter with longest remaining deadline, skip expired.
  Waiter* selectNextFiller(PendingKey& pk);

  // Resume all remaining waiters with TimedOut status and clean up.
  void releaseAll(const std::string& key);

  std::mutex mu_;
  std::shared_ptr<CacheStore> store_;
  size_t max_fill_retries_;
  std::unordered_map<std::string, PendingKey> pending_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
