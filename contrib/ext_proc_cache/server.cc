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

ExtProcCacheReactor::ExtProcCacheReactor() { run(); }

void ExtProcCacheReactor::OnReadDone(bool ok) {
  read_ok_ = ok;
  read_handle_.resume();
}

void ExtProcCacheReactor::OnWriteDone(bool ok) {
  write_ok_ = ok;
  write_handle_.resume();
}

void ExtProcCacheReactor::OnDone() { delete this; }

Task ExtProcCacheReactor::run() {
  while (true) {
    bool read_ok = co_await ReadAwaitable{this, &request_};
    if (!read_ok) {
      break;
    }

    response_ = buildResponse(request_);

    bool write_ok = co_await WriteAwaitable{this, &response_};
    if (!write_ok) {
      break;
    }
  }
  Finish(grpc::Status::OK);
}

ProcessingResponse ExtProcCacheReactor::buildResponse(const ProcessingRequest& request) {
  ProcessingResponse response;
  if (request.has_request_headers()) {
    response.mutable_request_headers();
  } else if (request.has_response_headers()) {
    response.mutable_response_headers();
  } else if (request.has_request_body()) {
    response.mutable_request_body();
  } else if (request.has_response_body()) {
    response.mutable_response_body();
  } else if (request.has_request_trailers()) {
    response.mutable_request_trailers();
  } else if (request.has_response_trailers()) {
    response.mutable_response_trailers();
  }
  return response;
}

// --- ExtProcCacheService ---

grpc::ServerBidiReactor<ProcessingRequest, ProcessingResponse>*
ExtProcCacheService::Process(grpc::CallbackServerContext*) {
  return new ExtProcCacheReactor();
}

// --- ExtProcCacheServer ---

void ExtProcCacheServer::start(const std::string& address) {
  service_ = std::make_unique<ExtProcCacheService>();
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
