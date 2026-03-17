#pragma once

#include <algorithm>
#include <chrono>

#include "contrib/ext_proc_cache/cache_types.h"

#include "absl/strings/numbers.h"
#include "absl/time/time.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Abstract interface for calculating cache entry freshness/age.
class CacheAgeCalculator {
public:
  virtual ~CacheAgeCalculator() = default;
  virtual CacheEntryUsability calculateUsability(const ProtoHeaderMap& request_headers,
                                                 const ProtoHeaderMap& cached_response_headers,
                                                 uint64_t content_length,
                                                 SystemTime response_time, SystemTime now) = 0;
};

// Default age calculator implementing RFC 7234 freshness lifetime from
// max-age/s-maxage/Expires, age calculation, and request min-fresh/max-stale/max-age directives.
class DefaultCacheAgeCalculator : public CacheAgeCalculator {
public:
  CacheEntryUsability calculateUsability(const ProtoHeaderMap& request_headers,
                                         const ProtoHeaderMap& cached_response_headers,
                                         uint64_t /*content_length*/, SystemTime response_time,
                                         SystemTime now) override {
    CacheEntryUsability result;

    // Calculate age per RFC 7234 section 4.2.3.
    const Seconds age = calculateAge(cached_response_headers, response_time, now);
    result.age = age;

    // Parse response cache-control.
    const std::string cache_control = getHeader(cached_response_headers, "cache-control");
    ResponseCacheControl rcc(cache_control);

    // Parse request cache-control.
    const std::string req_cache_control = getHeader(request_headers, "cache-control");
    RequestCacheControl req_cc;
    if (!req_cache_control.empty()) {
      req_cc = RequestCacheControl(req_cache_control);
    }

    // Check if request or response requires validation.
    const auto response_age = age;
    const bool request_max_age_exceeded =
        req_cc.max_age_.has_value() &&
        std::chrono::duration_cast<Seconds>(*req_cc.max_age_) < response_age;

    if (rcc.must_validate_ || req_cc.must_validate_ || request_max_age_exceeded) {
      result.status = CacheEntryStatus::RequiresValidation;
      result.ttl = Seconds(0);
      return result;
    }

    // Calculate freshness lifetime.
    SystemTime::duration freshness_lifetime;
    if (rcc.max_age_.has_value()) {
      freshness_lifetime = *rcc.max_age_;
    } else {
      // Use Expires - Date.
      const SystemTime expires_value = parseHttpTime(getHeader(cached_response_headers, "expires"));
      const SystemTime date_value = parseHttpTime(getHeader(cached_response_headers, "date"));
      if (expires_value == SystemTime{} || date_value == SystemTime{}) {
        // Can't calculate freshness without both.
        result.status = CacheEntryStatus::Unusable;
        result.ttl = Seconds(0);
        return result;
      }
      freshness_lifetime = expires_value - date_value;
    }

    const auto freshness_lifetime_secs =
        std::chrono::duration_cast<Seconds>(freshness_lifetime);
    result.ttl = freshness_lifetime_secs - age;

    if (response_age > freshness_lifetime_secs) {
      // Response is stale.
      const bool allowed_by_max_stale =
          req_cc.max_stale_.has_value() &&
          std::chrono::duration_cast<Seconds>(*req_cc.max_stale_) >
              (response_age - freshness_lifetime_secs);
      if (rcc.no_stale_ || !allowed_by_max_stale) {
        result.status = CacheEntryStatus::RequiresValidation;
        return result;
      }
      // Stale but allowed by max-stale.
      result.status = CacheEntryStatus::Ok;
    } else {
      // Response is fresh. Check min-fresh.
      const bool min_fresh_unsatisfied =
          req_cc.min_fresh_.has_value() &&
          std::chrono::duration_cast<Seconds>(*req_cc.min_fresh_) >
              (freshness_lifetime_secs - response_age);
      if (min_fresh_unsatisfied) {
        result.status = CacheEntryStatus::RequiresValidation;
        return result;
      }
      result.status = CacheEntryStatus::Ok;
    }

    return result;
  }

private:
  // Calculate age per RFC 7234 section 4.2.3.
  static Seconds calculateAge(const ProtoHeaderMap& response_headers, SystemTime response_time,
                               SystemTime now) {
    const SystemTime date_value = parseHttpTime(getHeader(response_headers, "date"));

    long age_value = 0;
    const std::string age_header = getHeader(response_headers, "age");
    if (!age_header.empty()) {
      (void)absl::SimpleAtoi(age_header, &age_value);
    }

    const SystemTime::duration apparent_age =
        std::max(SystemTime::duration(0), response_time - date_value);
    const SystemTime::duration corrected_age_value = Seconds(age_value);
    const SystemTime::duration corrected_initial_age = std::max(apparent_age, corrected_age_value);
    const SystemTime::duration resident_time = now - response_time;
    const SystemTime::duration current_age = corrected_initial_age + resident_time;

    return std::chrono::duration_cast<Seconds>(current_age);
  }

  // Parse an HTTP date string into SystemTime.
  static SystemTime parseHttpTime(absl::string_view input) {
    if (input.empty()) {
      return {};
    }
    absl::Time time;
    // RFC 7231 date formats.
    static constexpr absl::string_view formats[] = {"%a, %d %b %Y %H:%M:%S GMT",
                                                    "%A, %d-%b-%y %H:%M:%S GMT",
                                                    "%a %b %e %H:%M:%S %Y"};
    for (absl::string_view format : formats) {
      if (absl::ParseTime(format, input, &time, nullptr)) {
        return absl::ToChronoTime(time);
      }
    }
    return {};
  }
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
