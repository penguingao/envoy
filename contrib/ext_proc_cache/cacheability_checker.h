#pragma once

#include "contrib/ext_proc_cache/cache_types.h"

#include "absl/container/flat_hash_set.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Abstract interface for determining request/response cacheability.
class CacheabilityChecker {
public:
  virtual ~CacheabilityChecker() = default;
  virtual RequestCacheability requestCacheability(const ProtoHeaderMap& request_headers) = 0;
  virtual ResponseCacheability responseCacheability(const ProtoHeaderMap& request_headers,
                                                    const ProtoHeaderMap& response_headers) = 0;
};

// Default cacheability checker implementing RFC 7234 rules.
// Only GET, only specific status codes, respects no-store/private/Authorization.
class DefaultCacheabilityChecker : public CacheabilityChecker {
public:
  RequestCacheability requestCacheability(const ProtoHeaderMap& request_headers) override {
    const std::string method = getHeader(request_headers, ":method");
    // Only cache GET requests.
    if (method != "GET") {
      return RequestCacheability::Bypass;
    }

    // Must have :authority and :path.
    if (!hasHeader(request_headers, ":authority") || !hasHeader(request_headers, ":path")) {
      return RequestCacheability::Bypass;
    }

    // Must have valid scheme.
    const std::string scheme = getHeader(request_headers, ":scheme");
    if (scheme != "http" && scheme != "https") {
      return RequestCacheability::Bypass;
    }

    // Authorization header without public cache-control -> bypass.
    if (hasHeader(request_headers, "authorization")) {
      return RequestCacheability::Bypass;
    }

    // Check request cache-control.
    const std::string cache_control = getHeader(request_headers, "cache-control");
    if (!cache_control.empty()) {
      RequestCacheControl rcc(cache_control);
      if (rcc.no_store_) {
        return RequestCacheability::NoStore;
      }
    }

    return RequestCacheability::Cacheable;
  }

  ResponseCacheability responseCacheability(const ProtoHeaderMap& /*request_headers*/,
                                            const ProtoHeaderMap& response_headers) override {
    const std::string status = getHeader(response_headers, ":status");
    if (!cacheableStatusCodes().contains(status)) {
      return ResponseCacheability::DoNotStore;
    }

    const std::string cache_control = getHeader(response_headers, "cache-control");
    ResponseCacheControl rcc(cache_control);

    if (rcc.no_store_) {
      return ResponseCacheability::DoNotStore;
    }

    // Must have enough data to calculate freshness lifetime.
    const bool has_validation_data = rcc.must_validate_ || rcc.max_age_.has_value() ||
                                     (hasHeader(response_headers, "expires") &&
                                      hasHeader(response_headers, "date"));
    if (!has_validation_data) {
      return ResponseCacheability::DoNotStore;
    }

    return ResponseCacheability::StoreFullResponse;
  }

private:
  static const absl::flat_hash_set<std::string>& cacheableStatusCodes() {
    static const auto* codes =
        new absl::flat_hash_set<std::string>({"200", "301", "308"});
    return *codes;
  }
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
