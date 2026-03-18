#include "contrib/ext_proc_cache/cache_lookup_coordinator.h"

#include <algorithm>
#include <chrono>

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

CacheLookupCoordinator::CacheLookupCoordinator(std::shared_ptr<CacheStore> store,
                                               size_t max_fill_retries)
    : store_(std::move(store)), max_fill_retries_(max_fill_retries) {}

Awaitable<CoordinatedLookupResult> CacheLookupCoordinator::lookup(
    const std::string& key, std::chrono::system_clock::time_point deadline) {
  // First, check the cache store.
  auto cached = co_await store_->lookup(key);
  if (cached.has_value()) {
    auto& result = *cached;
    co_return CoordinatedLookupResult{
        LookupStatus::Hit, std::move(result.metadata),
        result.body_reader_factory->createReader()};
  }

  // Cache miss. Check if there's already a fill in progress.
  CoordinatedLookupResult result;
  bool is_filler = false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto& pk = pending_[key];

    if (!pk.filling) {
      // No fill in progress — this stream becomes the filler.
      pk.filling = true;
      pk.fill_attempts = 1;
      is_filler = true;
    }
  }

  if (is_filler) {
    co_return CoordinatedLookupResult{LookupStatus::YouFill, std::nullopt, nullptr};
  }

  // Another stream is filling. Check deadline.
  auto now = std::chrono::system_clock::now();
  if (now >= deadline) {
    co_return CoordinatedLookupResult{LookupStatus::TimedOut, std::nullopt, nullptr};
  }

  // Suspend and wait for the fill to complete.
  // The coroutine will be resumed by reportFillSuccess/reportFillFailure/cancelWaiter.
  struct SuspendAwaitable {
    CacheLookupCoordinator* coordinator;
    const std::string& key;
    std::chrono::system_clock::time_point deadline;
    CoordinatedLookupResult result;

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
      std::lock_guard<std::mutex> lock(coordinator->mu_);
      auto it = coordinator->pending_.find(key);
      if (it == coordinator->pending_.end() || !it->second.filling) {
        // Fill completed between our check and suspend. Resume immediately.
        result = {LookupStatus::TimedOut, std::nullopt, nullptr};
        h.resume();
        return;
      }
      it->second.waiters.push_back(Waiter{h, deadline, &result});
    }

    CoordinatedLookupResult await_resume() { return std::move(result); }
  };

  co_return co_await SuspendAwaitable{this, key, deadline, {}};
}

void CacheLookupCoordinator::reportFillSuccess(
    const std::string& key, const CacheEntryMetadata& metadata,
    std::shared_ptr<CacheBodyReaderFactory> factory) {
  std::vector<Waiter> waiters_to_resume;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
      return;
    }
    waiters_to_resume = std::move(it->second.waiters);
    pending_.erase(it);
  }

  // Resume all waiters with metadata and an independent body reader each.
  for (auto& waiter : waiters_to_resume) {
    *waiter.result_ptr = CoordinatedLookupResult{
        LookupStatus::Hit, CacheEntryMetadata(metadata), factory->createReader()};
    waiter.handle.resume();
  }
}

void CacheLookupCoordinator::reportFillFailure(const std::string& key) {
  Waiter* next_filler = nullptr;
  std::vector<Waiter> waiters_to_timeout;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
      return;
    }
    auto& pk = it->second;
    pk.filling = false;

    if (pk.fill_attempts < max_fill_retries_ && !pk.waiters.empty()) {
      next_filler = selectNextFiller(pk);
      if (next_filler) {
        pk.filling = true;
        pk.fill_attempts++;
        // Copy out the filler info before modifying the vector.
        Waiter filler_copy = *next_filler;
        // Remove filler from waiters.
        pk.waiters.erase(
            std::remove_if(pk.waiters.begin(), pk.waiters.end(),
                           [next_filler](const Waiter& w) { return &w == next_filler; }),
            pk.waiters.end());
        // Resume filler outside the lock.
        *filler_copy.result_ptr = CoordinatedLookupResult{LookupStatus::YouFill, std::nullopt, nullptr};
        filler_copy.handle.resume();
        return;
      }
    }

    // All retries exhausted or no live waiters. Release all.
    waiters_to_timeout = std::move(pk.waiters);
    pending_.erase(it);
  }

  for (auto& waiter : waiters_to_timeout) {
    *waiter.result_ptr = CoordinatedLookupResult{LookupStatus::TimedOut, std::nullopt, nullptr};
    waiter.handle.resume();
  }
}

void CacheLookupCoordinator::cancelWaiter(const std::string& key, std::coroutine_handle<> handle) {
  bool was_filling = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
      return;
    }
    auto& pk = it->second;
    auto wit = std::find_if(pk.waiters.begin(), pk.waiters.end(),
                            [handle](const Waiter& w) { return w.handle == handle; });
    if (wit != pk.waiters.end()) {
      *wit->result_ptr = CoordinatedLookupResult{LookupStatus::Cancelled, std::nullopt, nullptr};
      wit->handle.resume();
      pk.waiters.erase(wit);
    }
  }

  // If this waiter was the filler, treat as fill failure.
  // Note: the filler is not in the waiters list, so this path handles
  // cancellation of the filler stream via the CacheStreamHandler.
  if (was_filling) {
    reportFillFailure(key);
  }
}

CacheLookupCoordinator::Waiter*
CacheLookupCoordinator::selectNextFiller(PendingKey& pk) {
  auto now = std::chrono::system_clock::now();
  Waiter* best = nullptr;
  for (auto& w : pk.waiters) {
    if (w.deadline <= now) {
      continue; // expired
    }
    if (!best || w.deadline > best->deadline) {
      best = &w;
    }
  }
  return best;
}

void CacheLookupCoordinator::releaseAll(const std::string& key) {
  std::vector<Waiter> waiters_to_release;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
      return;
    }
    waiters_to_release = std::move(it->second.waiters);
    pending_.erase(it);
  }

  for (auto& waiter : waiters_to_release) {
    *waiter.result_ptr = CoordinatedLookupResult{LookupStatus::TimedOut, std::nullopt, nullptr};
    waiter.handle.resume();
  }
}

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
