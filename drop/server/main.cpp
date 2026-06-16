#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "common.pb.h"
#include "healthcheck.grpc.pb.h"
#include "hotmethod.grpc.pb.h"
#include "control.grpc.pb.h"
#include "init.grpc.pb.h"

#include <glog/logging.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

// ── HealthCheckService ──────────────────────────────────────────
class HealthCheckServiceImpl final
    : public drop::healthcheck::HealthCheck::Service {
  Status Do(ServerContext* /*ctx*/,
            const drop::healthcheck::HealthCheckRequest* /*req*/,
            drop::healthcheck::HealthCheckResponse* resp) override {
    resp->set_status(drop::healthcheck::HealthCheckResponse::SERVING);
    resp->set_pending(false);
    return Status::OK;
  }
};

// ── HotmethodService ────────────────────────────────────────────
class HotmethodServiceImpl final
    : public drop::hotmethod::Hotmethod::Service {
  Status Collect(ServerContext* /*ctx*/,
                 const drop::hotmethod::Target* /*req*/,
                 google::protobuf::Empty* /*resp*/) override {
    return Status(StatusCode::UNIMPLEMENTED, "not yet implemented");
  }

  Status NotifyResult(ServerContext* /*ctx*/,
                      const drop::hotmethod::TaskResult* /*req*/,
                      google::protobuf::Empty* /*resp*/) override {
    return Status(StatusCode::UNIMPLEMENTED, "not yet implemented");
  }
};

// ── ControlService ──────────────────────────────────────────────
class ControlServiceImpl final
    : public drop::control::Control::Service {
  Status CreateTask(ServerContext* /*ctx*/,
                    const drop::control::CreateTaskRequest* /*req*/,
                    drop::control::CreateTaskResponse* resp) override {
    resp->set_success(false);
    resp->set_message("not yet implemented");
    return Status::OK;
  }

  Status FetchData(ServerContext* /*ctx*/,
                   const drop::control::FetchDataRequest* /*req*/,
                   drop::control::FetchDataResponse* resp) override {
    resp->set_success(false);
    resp->set_error_message("not yet implemented");
    return Status::OK;
  }

  Status StatAgent(ServerContext* /*ctx*/,
                   const drop::control::StatAgentRequest* /*req*/,
                   drop::control::StatAgentResponse* /*resp*/) override {
    return Status(StatusCode::UNIMPLEMENTED, "not yet implemented");
  }
};

// ── InitService ─────────────────────────────────────────────────
class InitServiceImpl final
    : public drop::init::Init::Service {
  Status RegisterAgent(ServerContext* /*ctx*/,
                       const drop::init::RegisterAgentRequest* /*req*/,
                       drop::init::RegisterAgentResponse* resp) override {
    resp->set_success(false);
    resp->set_message("not yet implemented");
    return Status::OK;
  }

  Status FetchConfig(ServerContext* /*ctx*/,
                     const drop::init::FetchConfigRequest* /*req*/,
                     drop::init::FetchConfigResponse* resp) override {
    return Status(StatusCode::UNIMPLEMENTED, "not yet implemented");
  }
};

// ── main ────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);

  int port = 50051;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-port" && i + 1 < argc) {
      port = std::stoi(argv[++i]);
    }
  }

  std::string addr = std::string("0.0.0.0:") + std::to_string(port);

  HealthCheckServiceImpl hc;
  HotmethodServiceImpl   hm;
  ControlServiceImpl     ctrl;
  InitServiceImpl        init_svc;

  grpc::EnableDefaultHealthCheckService(true);

  ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&hc);
  builder.RegisterService(&hm);
  builder.RegisterService(&ctrl);
  builder.RegisterService(&init_svc);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  LOG(INFO) << "drop_server listening on " << addr;

  server->Wait();
  return 0;
}
