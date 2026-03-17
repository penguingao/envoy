#include <csignal>
#include <iostream>
#include <string>

#include "contrib/ext_proc_cache/server.h"

std::function<void(int)> shutdown_handler;

void signal_handler(int signal) {
  if (shutdown_handler) {
    shutdown_handler(signal);
  }
}

int main(int argc, char** argv) {
  std::string address = "0.0.0.0:50051";
  if (argc > 1) {
    address = argv[1];
  }

  Envoy::Extensions::ExtProcCache::ExtProcCacheServer server;
  server.start(address);

  std::cout << "ExtProcCache server listening on " << address
            << " (port " << server.port() << ")" << std::endl;

  shutdown_handler = [&server](int) {
    std::cout << "Shutting down..." << std::endl;
    server.shutdown();
  };

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Block until Shutdown() is called by the signal handler.
  server.wait();

  return 0;
}
