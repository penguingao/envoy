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
                                       std::chrono::system_clock::time_point deadline,
                                       size_t chunk_size)
    : coordinator_(std::move(coordinator)), key_gen_(std::move(key_gen)),
      cacheability_(std::move(cacheability)), age_calc_(std::move(age_calc)),
      deadline_(deadline), chunk_size_(chunk_size) {}

HandleResult
CacheStreamHandler::buildCacheHitResult(const CacheEntryMetadata& metadata,
                                        std::unique_ptr<CacheBodyReader> reader,
                                        std::optional<Seconds> age) {
  HandleResult result;
  auto* streamed = result.response.mutable_streamed_immediate_response();
  auto* http_headers = streamed->mutable_headers_response();
  auto* header_map = http_headers->mutable_headers();

  // Add :status pseudo-header.
  auto* status_h = header_map->add_headers();
  status_h->set_key(":status");
  status_h->set_raw_value(std::to_string(metadata.status_code));

  // Add cached response headers (skip pseudo-headers already in the entry).
  for (const auto& header : metadata.response_headers.headers()) {
    if (!header.key().empty() && header.key()[0] == ':') {
      continue;
    }
    auto* h = header_map->add_headers();
    h->set_key(header.key());
    h->set_raw_value(header.raw_value());
  }

  // Add Age header if provided.
  if (age.has_value()) {
    auto* age_h = header_map->add_headers();
    age_h->set_key("age");
    age_h->set_raw_value(std::to_string(age->count()));
  }

  // If body is empty, set end_of_stream on headers and skip body streaming.
  if (metadata.content_length == 0) {
    http_headers->set_end_of_stream(true);
    return result;
  }

  // Pass reader to the reactor for lazy chunk-by-chunk streaming.
  result.body_reader = std::move(reader);
  result.chunk_size = (chunk_size_ > 0) ? chunk_size_ : metadata.content_length;
  return result;
}

ProcessingResponse
CacheStreamHandler::buildImmediateCacheResponse(const CacheEntryMetadata& metadata,
                                                const std::string& body) {
  ProcessingResponse response;
  auto* immediate = response.mutable_immediate_response();
  immediate->mutable_status()->set_code(
      static_cast<envoy::type::v3::StatusCode>(metadata.status_code));
  immediate->set_body(body);

  auto* header_mutation = immediate->mutable_headers();
  for (const auto& header : metadata.response_headers.headers()) {
    if (!header.key().empty() && header.key()[0] == ':') {
      continue;
    }
    auto* hvo = header_mutation->add_set_headers();
    hvo->mutable_header()->set_key(header.key());
    hvo->mutable_header()->set_raw_value(header.raw_value());
  }

  return response;
}

Awaitable<HandleResult>
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
    co_return HandleResult{std::move(response), nullptr, 0};
  }

  // Coordinated lookup.
  auto lookup_result = co_await coordinator_->lookup(current_key_, deadline_);

  std::cerr << "[HANDLER] lookup status=" << static_cast<int>(lookup_result.status)
            << " has_entry=" << lookup_result.metadata.has_value() << std::endl;

  switch (lookup_result.status) {
  case LookupStatus::Hit: {
    auto& metadata = *lookup_result.metadata;

    // Check freshness.
    auto now = std::chrono::system_clock::now();
    auto usability = age_calc_->calculateUsability(saved_request_headers_,
                                                   metadata.response_headers,
                                                   metadata.content_length,
                                                   metadata.response_time, now);

    std::cerr << "[HANDLER] usability: status=" << static_cast<int>(usability.status)
              << " age=" << usability.age.count()
              << " ttl=" << usability.ttl.count() << std::endl;
    if (usability.status == CacheEntryStatus::Ok) {
      // Serve from cache via StreamedImmediateResponse.
      co_return buildCacheHitResult(metadata, std::move(lookup_result.body_reader),
                                    usability.age);
    }

    // Entry is stale/requires validation — forward to upstream with
    // conditional headers so the origin can respond with 304.
    validating_ = true;
    stale_metadata_ = std::move(metadata);
    stale_reader_ = std::move(lookup_result.body_reader);

    auto* headers_resp = response.mutable_request_headers();
    auto* mutation = headers_resp->mutable_response()->mutable_header_mutation();

    // Add If-None-Match from cached ETag.
    const std::string etag = getHeader(stale_metadata_->response_headers, "etag");
    if (!etag.empty()) {
      auto* hvo = mutation->add_set_headers();
      hvo->mutable_header()->set_key("if-none-match");
      hvo->mutable_header()->set_raw_value(etag);
    }

    // Add If-Modified-Since from cached Last-Modified.
    const std::string last_modified =
        getHeader(stale_metadata_->response_headers, "last-modified");
    if (!last_modified.empty()) {
      auto* hvo = mutation->add_set_headers();
      hvo->mutable_header()->set_key("if-modified-since");
      hvo->mutable_header()->set_raw_value(last_modified);
    }

    co_return HandleResult{std::move(response), nullptr, 0};
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
    co_return HandleResult{std::move(response), nullptr, 0};

  case LookupStatus::TimedOut:
  case LookupStatus::Cancelled:
    // Proceed without caching.
    response.mutable_request_headers();
    co_return HandleResult{std::move(response), nullptr, 0};
  }

  // Unreachable, but satisfy compiler.
  response.mutable_request_headers();
  co_return HandleResult{std::move(response), nullptr, 0};
}

Awaitable<HandleResult>
CacheStreamHandler::onResponseHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers) {
  ProcessingResponse response;

  const auto& proto_headers = headers.headers();
  const std::string status_str = getHeader(proto_headers, ":status");

  std::cerr << "[HANDLER] onResponseHeaders: is_filler_=" << is_filler_
            << " validating_=" << validating_ << " status=" << status_str << std::endl;

  // Handle 304 Not Modified during conditional revalidation.
  if (validating_ && status_str == "304" && stale_metadata_.has_value()) {
    validating_ = false;
    auto& metadata = *stale_metadata_;

    std::cerr << "[HANDLER] 304 revalidation: refreshing cached headers" << std::endl;

    // Per RFC 7234 section 4.3.4: update stored headers with 304 headers.
    // Use raw_value to stay consistent with ext_proc header encoding.
    for (const auto& h : proto_headers.headers()) {
      // Skip pseudo-headers (e.g. :status from the 304).
      if (!h.key().empty() && h.key()[0] == ':') {
        continue;
      }
      const std::string value =
          !h.raw_value().empty() ? std::string(h.raw_value()) : std::string(h.value());
      // Find and update existing header, or add new one.
      bool found = false;
      for (auto& existing : *metadata.response_headers.mutable_headers()) {
        if (existing.key() == h.key()) {
          existing.set_raw_value(value);
          existing.set_value(value);
          found = true;
          break;
        }
      }
      if (!found) {
        auto* new_header = metadata.response_headers.add_headers();
        new_header->set_key(h.key());
        new_header->set_raw_value(value);
        new_header->set_value(value);
      }
    }
    metadata.response_time = std::chrono::system_clock::now();

    // Read the full body from the stale reader for re-storing and serving.
    auto body = co_await stale_reader_->readAll();

    // Store the refreshed entry.
    CachedEntry refreshed_entry;
    refreshed_entry.response_headers = metadata.response_headers;
    refreshed_entry.trailers = metadata.trailers;
    refreshed_entry.response_time = metadata.response_time;
    refreshed_entry.status_code = metadata.status_code;
    refreshed_entry.body = body;
    auto store_ok = co_await coordinator_->store()->store(current_key_, refreshed_entry);
    std::cerr << "[HANDLER] 304 store result: " << store_ok << std::endl;

    // Serve the cached body with refreshed headers via ImmediateResponse.
    // (StreamedImmediateResponse is only valid in response to request_headers.)
    auto imm = buildImmediateCacheResponse(metadata, body);
    stale_metadata_.reset();
    stale_reader_.reset();
    co_return HandleResult{std::move(imm), nullptr, 0};
  }

  // Non-304 response during validation — discard stale entry, pass through.
  if (validating_) {
    validating_ = false;
    stale_metadata_.reset();
    stale_reader_.reset();
  }

  response.mutable_response_headers();

  if (!is_filler_) {
    co_return HandleResult{std::move(response), nullptr, 0};
  }

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
    int status_int = 0;
    (void)absl::SimpleAtoi(status_str, &status_int);
    pending_entry_.status_code = static_cast<uint32_t>(status_int);

    // In FULL_DUPLEX_STREAMED mode, body chunks arrive streamed.
    // We accumulate them for caching in onResponseBody() and pass them
    // through via StreamedBodyResponse.
  } else {
    // Not storing — report fill failure so the coordinator can promote the
    // next waiter to filler.
    is_filler_ = false;
    coordinator_->reportFillFailure(current_key_);

    // For server errors (5xx), wait for a retry filler to succeed rather than
    // forwarding the error to the downstream client.
    int status_int = 0;
    (void)absl::SimpleAtoi(status_str, &status_int);
    if (status_int >= 500) {
      std::cerr << "[HANDLER] Server error " << status_int
                << ", waiting for retry filler" << std::endl;

      // Re-enqueue as a waiter by calling lookup() again. If a retry filler
      // is in progress, we'll suspend until it completes.
      auto retry_result = co_await coordinator_->lookup(current_key_, deadline_);

      if (retry_result.status == LookupStatus::Hit) {
        const auto& metadata = *retry_result.metadata;
        std::cerr << "[HANDLER] Retry succeeded, serving from cache" << std::endl;

        // Read the full body and serve via ImmediateResponse.
        // (StreamedImmediateResponse is only valid in response to request_headers.)
        auto body = co_await retry_result.body_reader->readAll();
        co_return HandleResult{buildImmediateCacheResponse(metadata, body), nullptr, 0};
      }

      if (retry_result.status == LookupStatus::YouFill) {
        // No fill in progress — give up the accidental filler role.
        coordinator_->reportFillFailure(current_key_);
      }

      // Retry failed or timed out — fall through with original error.
      std::cerr << "[HANDLER] Retry failed, passing through error" << std::endl;
    }
  }

  co_return HandleResult{std::move(response), nullptr, 0};
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
      CacheEntryMetadata metadata;
      metadata.response_headers = pending_entry_.response_headers;
      metadata.trailers = pending_entry_.trailers;
      metadata.response_time = pending_entry_.response_time;
      metadata.status_code = pending_entry_.status_code;
      metadata.content_length = pending_entry_.body.size();
      auto factory =
          coordinator_->store()->createBodyReaderFactory(std::move(pending_entry_.body));
      coordinator_->reportFillSuccess(current_key_, metadata, std::move(factory));
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
    CacheEntryMetadata metadata;
    metadata.response_headers = pending_entry_.response_headers;
    metadata.trailers = pending_entry_.trailers;
    metadata.response_time = pending_entry_.response_time;
    metadata.status_code = pending_entry_.status_code;
    metadata.content_length = pending_entry_.body.size();
    auto factory =
        coordinator_->store()->createBodyReaderFactory(std::move(pending_entry_.body));
    coordinator_->reportFillSuccess(current_key_, metadata, std::move(factory));
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
