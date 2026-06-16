#include <iostream>
#include <string>

#include <gflags/gflags.h>
#include <glog/logging.h>

DEFINE_string(config, "", "Path to agent config JSON");
DEFINE_bool(foreground, false, "Run in foreground (do not daemonize)");
DEFINE_int32(port, 50051, "drop_server gRPC port");

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  LOG(INFO) << "drop_agent starting";
  LOG(INFO) << "  config      = " << FLAGS_config;
  LOG(INFO) << "  foreground  = " << FLAGS_foreground;
  LOG(INFO) << "  server_port = " << FLAGS_port;

  // TODO: load config, daemonize if !foreground, start heartbeat + worker threads

  LOG(INFO) << "drop_agent exiting (shell mode, no server connection yet)";
  return 0;
}
