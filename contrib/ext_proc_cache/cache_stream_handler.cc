#include "contrib/ext_proc_cache/cache_stream_handler.h"

#include "envoy/service/ext_proc/v3/external_processor.pb.h"

#include <iostream>

#include "absl/strings/numbers.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

CacheStreamHandler::CacheStreamHandler(std::shared_ptr<CacheLookupCoordinator> coordinator,
                                       std::shared_ptr<CacheKeyGenerator> key_gen,
                                       std::shared_ptr<CacheabilityChecker> cacheability,
                                       std::shared_ptr<CacheAgeCalculator> age_calc,
                                       std::chrono::system_clock::time_point deadline)
    : coordinator_(std::move(coordinator)), key_gen_(std::move(key_gen)),
      cacheability_(std::move(cacheability)), age_calc_(std::move(age_calc)), deadline_(deadline) {}

Awaitable<ProcessingResponse>
CacheStreamHandler::onRequestHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers) {
  ProcessingResponse response;
  const auto& proto_headers = headers.headers();
  saved_request_headers_ = proto_headers;

  // Generate cache key.
  current_key_ = key_gen_->generateKey(proto_headers);
  std::cerr << "[HANDLER] onRequestHeaders: key=" << current_key_ << std::endl;
  for (const auto& h : proto_headers.headers()) {
    std::cerr << "[HANDLER]   " << h.key() << ": " << h.value() << std::endl;
  }

  // Check request cacheability.
  auto req_cacheability = cacheability_->requestCacheability(proto_headers);
  std::cerr << "[HANDLER] req_cacheability=" << static_cast<int>(req_cacheability) << std::endl;
  if (req_cacheability == RequestCacheability::Bypass) {
    // Not cacheable — just continue.
    response.mutable_request_headers();
    co_return response;
  }

  // Coordinated lookup.
  auto lookup_result = co_await coordinator_->lookup(current_key_, deadline_);

  std::cerr << "[HANDLER] lookup status=" << static_cast<int>(lookup_result.status)
            << " has_entry=" << lookup_result.entry.has_value() << std::endl;

  switch (lookup_result.status) {
  case LookupStatus::Hit: {
    const auto& entry = *lookup_result.entry;

    // Check freshness.
    auto now = std::chrono::system_clock::now();
    auto usability = age_calc_->calculateUsability(saved_request_headers_,
                                                   entry.response_headers, entry.body.size(),
                                                   entry.response_time, now);

    std::cerr << "[HANDLER] usability: status=" << static_cast<int>(usability.status)
              << " age=" << usability.age.count()
              << " ttl=" << usability.ttl.count() << std::endl;
    if (usability.status == CacheEntryStatus::Ok) {
      // Serve from cache via ImmediateResponse.
      auto* immediate = response.mutable_immediate_response();
      immediate->mutable_status()->set_code(
          static_cast<envoy::type::v3::StatusCode>(entry.status_code));
      immediate->set_body(entry.body);

      // Set headers from cached response.
      auto* header_mutation = immediate->mutable_headers();
      for (const auto& header : entry.response_headers.headers()) {
        if (!header.key().empty() && header.key()[0] == ':') {
          continue;
        }
        auto* hvo = header_mutation->add_set_headers();
        hvo->mutable_header()->set_key(header.key());
        hvo->mutable_header()->set_raw_value(header.value());
      }

      // Set Age header.
      auto* age_hvo = header_mutation->add_set_headers();
      age_hvo->mutable_header()->set_key("age");
      age_hvo->mutable_header()->set_raw_value(std::to_string(usability.age.count()));

      co_return response;
    }

    // Entry is stale/requires validation — fall through to upstream.
    response.mutable_request_headers();
    co_return response;
  }

  case LookupStatus::YouFill:
    is_filler_ = true;
    if (req_cacheability == RequestCacheability::NoStore) {
      // Don't store the response, but we were designated filler.
      // Report fill failure so coordinator can pick another.
      coordinator_->reportFillFailure(current_key_);
      is_filler_ = false;
    }
    response.mutable_request_headers();
    co_return response;

  case LookupStatus::TimedOut:
  case LookupStatus::Cancelled:
    // Proceed without caching.
    response.mutable_request_headers();
    co_return response;
  }

  // Unreachable, but satisfy compiler.
  response.mutable_request_headers();
  co_return response;
}

ProcessingResponse
CacheStreamHandler::onResponseHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers) {
  ProcessingResponse response;
  response.mutable_response_headers();

  std::cerr << "[HANDLER] onResponseHeaders: is_filler_=" << is_filler_ << std::endl;

  if (!is_filler_) {
    return response;
  }

  const auto& proto_headers = headers.headers();
  std::cerr << "[HANDLER] Response headers count: " << proto_headers.headers_size() << std::endl;
  for (const auto& h : proto_headers.headers()) {
    std::cerr << "[HANDLER]   " << h.key() << ": " << h.value() << std::endl;
  }

  // Check response cacheability.
  auto resp_cacheability =
      cacheability_->responseCacheability(saved_request_headers_, proto_headers);

  std::cerr << "[HANDLER] resp_cacheability=" << static_cast<int>(resp_cacheability) << std::endl;

  if (resp_cacheability == ResponseCacheability::StoreFullResponse) {
    storing_ = true;
    pending_entry_.response_headers = proto_headers;
    pending_entry_.response_time = std::chrono::system_clock::now();

    // Extract status code from :status pseudo-header.
    const std::string status_str = getHeader(proto_headers, ":status");
    int status_int = 0;
    (void)absl::SimpleAtoi(status_str, &status_int);
    pending_entry_.status_code = static_cast<uint32_t>(status_int);

    // In FULL_DUPLEX_STREAMED mode, body chunks arrive streamed.
    // We accumulate them for caching in onResponseBody() and pass them
    // through via StreamedBodyResponse.
  } else {
    // Not storing — report fill failure.
    coordinator_->reportFillFailure(current_key_);
    is_filler_ = false;
  }

  return response;
}

Awaitable<ProcessingResponse>
CacheStreamHandler::onResponseBody(const envoy::service::ext_proc::v3::HttpBody& body) {
  ProcessingResponse response;

  std::cerr << "[HANDLER] onResponseBody: storing_=" << storing_
            << " body_size=" << body.body().size()
            << " end_of_stream=" << body.end_of_stream() << std::endl;

  // In FULL_DUPLEX_STREAMED mode, use StreamedBodyResponse to pass the body
  // through to the downstream/upstream.
  auto* body_mutation =
      response.mutable_response_body()->mutable_response()->mutable_body_mutation();
  auto* streamed = body_mutation->mutable_streamed_response();
  streamed->set_body(body.body());
  streamed->set_end_of_stream(body.end_of_stream());

  if (!storing_) {
    co_return response;
  }

  pending_entry_.body.append(body.body());

  if (body.end_of_stream()) {
    // Store the entry.
    std::cerr << "[HANDLER] Storing entry: key=" << current_key_
              << " body_size=" << pending_entry_.body.size()
              << " status_code=" << pending_entry_.status_code << std::endl;
    auto store_ok =
        co_await coordinator_->store()->store(current_key_, pending_entry_);
    std::cerr << "[HANDLER] Store result: " << store_ok << std::endl;
    if (store_ok) {
      coordinator_->reportFillSuccess(current_key_, pending_entry_);
    } else {
      coordinator_->reportFillFailure(current_key_);
    }
    storing_ = false;
    is_filler_ = false;
  }

  co_return response;
}

Awaitable<ProcessingResponse>
CacheStreamHandler::onResponseTrailers(const envoy::service::ext_proc::v3::HttpTrailers& trailers) {
  ProcessingResponse response;
  response.mutable_response_trailers();

  if (!storing_) {
    co_return response;
  }

  pending_entry_.trailers = trailers.trailers();

  // Finalize and store.
  auto store_ok =
      co_await coordinator_->store()->store(current_key_, pending_entry_);
  if (store_ok) {
    coordinator_->reportFillSuccess(current_key_, pending_entry_);
  } else {
    coordinator_->reportFillFailure(current_key_);
  }
  storing_ = false;
  is_filler_ = false;

  co_return response;
}

void CacheStreamHandler::onCancel() {
  if (is_filler_) {
    coordinator_->reportFillFailure(current_key_);
    is_filler_ = false;
  }
  storing_ = false;
}

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
