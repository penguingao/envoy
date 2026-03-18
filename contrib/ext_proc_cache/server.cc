#include "contrib/ext_proc_cache/server.h"

#include "grpc++/server_builder.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// --- ReadAwaitable ---

void ExtProcCacheReactor::ReadAwaitable::await_suspend(std::coroutine_handle<> h) {
  reactor->read_handle_ = h;
  reactor->StartRead(request);
}

bool ExtProcCacheReactor::ReadAwaitable::await_resume() const { return reactor->read_ok_; }

// --- WriteAwaitable ---

void ExtProcCacheReactor::WriteAwaitable::await_suspend(std::coroutine_handle<> h) {
  reactor->write_handle_ = h;
  reactor->StartWrite(response);
}

bool ExtProcCacheReactor::WriteAwaitable::await_resume() const { return reactor->write_ok_; }

// --- ExtProcCacheReactor ---

ExtProcCacheReactor::ExtProcCacheReactor(std::shared_ptr<CacheLookupCoordinator> coordinator,
                                         std::shared_ptr<CacheKeyGenerator> key_gen,
                                         std::shared_ptr<CacheabilityChecker> cacheability,
                                         std::shared_ptr<CacheAgeCalculator> age_calc,
                                         grpc::CallbackServerContext* context,
                                         size_t chunk_size) {
  // Extract deadline from the gRPC context.
  auto deadline = context->deadline();

  handler_ = std::make_unique<CacheStreamHandler>(std::move(coordinator), std::move(key_gen),
                                                  std::move(cacheability), std::move(age_calc),
                                                  deadline, chunk_size);
  run();
}

void ExtProcCacheReactor::OnReadDone(bool ok) {
  read_ok_ = ok;
  read_handle_.resume();
}

void ExtProcCacheReactor::OnWriteDone(bool ok) {
  write_ok_ = ok;
  write_handle_.resume();
}

void ExtProcCacheReactor::OnCancel() {
  if (handler_) {
    handler_->onCancel();
  }
}

void ExtProcCacheReactor::OnDone() { delete this; }

Task ExtProcCacheReactor::run() {
  bool first_message = true;
  while (true) {
    bool read_ok = co_await ReadAwaitable{this, &request_};
    if (!read_ok) {
      std::cerr << "[EXT_PROC_CACHE] Read failed, finishing stream" << std::endl;
      break;
    }

    // Validate that the first message has FULL_DUPLEX_STREAMED mode for the
    // response body. The protocol_config is only present on the first request.
    if (first_message) {
      first_message = false;
      if (request_.has_protocol_config()) {
        const auto& config = request_.protocol_config();
        if (config.response_body_mode() !=
            envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::FULL_DUPLEX_STREAMED) {
          std::cerr << "[EXT_PROC_CACHE] Error: response_body_mode must be FULL_DUPLEX_STREAMED, "
                    << "got " << config.response_body_mode() << std::endl;
          Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "ext_proc_cache requires response_body_mode to be "
                              "FULL_DUPLEX_STREAMED"));
          co_return;
        }
      }
    }

    std::cerr << "[EXT_PROC_CACHE] Received request: " << request_.request_case() << std::endl;
    auto responses = co_await handleRequest(request_);

    bool write_failed = false;
    for (auto& resp : responses) {
      response_ = std::move(resp);
      std::cerr << "[EXT_PROC_CACHE] Sending response: " << response_.response_case() << std::endl;

      bool write_ok = co_await WriteAwaitable{this, &response_};
      if (!write_ok) {
        std::cerr << "[EXT_PROC_CACHE] Write failed, finishing stream" << std::endl;
        write_failed = true;
        break;
      }
    }
    if (write_failed) {
      break;
    }
  }
  Finish(grpc::Status::OK);
}

Awaitable<std::vector<ProcessingResponse>>
ExtProcCacheReactor::handleRequest(const ProcessingRequest& request) {
  if (request.has_request_headers()) {
    co_return co_await handler_->onRequestHeaders(request.request_headers());
  } else if (request.has_response_headers()) {
    co_return co_await handler_->onResponseHeaders(request.response_headers());
  } else if (request.has_response_body()) {
    auto resp = co_await handler_->onResponseBody(request.response_body());
    co_return std::vector<ProcessingResponse>{std::move(resp)};
  } else if (request.has_response_trailers()) {
    auto resp = co_await handler_->onResponseTrailers(request.response_trailers());
    co_return std::vector<ProcessingResponse>{std::move(resp)};
  }

  // For request body/trailers or unknown types, just continue.
  ProcessingResponse response;
  if (request.has_request_body()) {
    response.mutable_request_body();
  } else if (request.has_request_trailers()) {
    response.mutable_request_trailers();
  }
  co_return std::vector<ProcessingResponse>{std::move(response)};
}

// --- ExtProcCacheService ---

ExtProcCacheService::ExtProcCacheService(std::shared_ptr<CacheLookupCoordinator> coordinator,
                                         std::shared_ptr<CacheKeyGenerator> key_gen,
                                         std::shared_ptr<CacheabilityChecker> cacheability,
                                         std::shared_ptr<CacheAgeCalculator> age_calc,
                                         size_t chunk_size)
    : coordinator_(std::move(coordinator)), key_gen_(std::move(key_gen)),
      cacheability_(std::move(cacheability)), age_calc_(std::move(age_calc)),
      chunk_size_(chunk_size) {}

grpc::ServerBidiReactor<ProcessingRequest, ProcessingResponse>*
ExtProcCacheService::Process(grpc::CallbackServerContext* context) {
  return new ExtProcCacheReactor(coordinator_, key_gen_, cacheability_, age_calc_, context,
                                chunk_size_);
}

// --- ExtProcCacheServer ---

void ExtProcCacheServer::start(const std::string& address,
                               std::shared_ptr<CacheLookupCoordinator> coordinator,
                               std::shared_ptr<CacheKeyGenerator> key_gen,
                               std::shared_ptr<CacheabilityChecker> cacheability,
                               std::shared_ptr<CacheAgeCalculator> age_calc,
                               size_t chunk_size) {
  service_ = std::make_unique<ExtProcCacheService>(
      std::move(coordinator), std::move(key_gen), std::move(cacheability), std::move(age_calc),
      chunk_size);
  grpc::ServerBuilder builder;
  builder.RegisterService(service_.get());
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &listening_port_);
  server_ = builder.BuildAndStart();
}

void ExtProcCacheServer::shutdown() {
  if (server_) {
    server_->Shutdown();
  }
}

void ExtProcCacheServer::wait() {
  if (server_) {
    server_->Wait();
  }
}

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
