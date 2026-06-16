#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "common.pb.h"
#include "healthcheck.grpc.pb.h"
#include "hotmethod.grpc.pb.h"
#include "control.grpc.pb.h"
#include "init.grpc.pb.h"

#include <glog/logging.h>
#include <gflags/gflags.h>

#include "task_queue.h"

#include <thread>
#include <chrono>
#include <vector>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

// ── gflags ──────────────────────────────────────────────────────────
DEFINE_int32(port, 50051, "drop_server gRPC listen port");
DEFINE_string(pg_dsn, "",
              "PostgreSQL connection string, e.g. "
              "\"host=127.0.0.1 port=5432 user=drop password=drop dbname=drop sslmode=disable\"");
DEFINE_int32(hb_timeout, 30, "Seconds without heartbeat before agent is marked offline");
DEFINE_int32(hb_scan_interval, 10, "Seconds between agent heartbeat scans");

// ── Shared state ────────────────────────────────────────────────────
static drop::TaskQueue       g_task_queue;
static drop::HeartbeatTracker g_hb_tracker;
static std::unique_ptr<drop::PGStore> g_pg;

// ── HealthCheckService ──────────────────────────────────────────────
class HealthCheckServiceImpl final
    : public drop::healthcheck::HealthCheck::Service {
  Status Do(ServerContext* /*ctx*/,
            const drop::healthcheck::HealthCheckRequest* req,
            drop::healthcheck::HealthCheckResponse* resp) override {
    const std::string& ip = req->ip_addr();

    // Track heartbeat
    g_hb_tracker.Update(ip);

    // Upsert agent_info in PG
    if (g_pg && g_pg->Connected()) {
      g_pg->UpsertAgent(ip, req->host_name(), req->uid(), req->agent_version());
    }

    // Check for pending tasks
    drop::hotmethod::TaskDesc desc;
    if (g_task_queue.Pop(ip, &desc)) {
      resp->set_pending(true);
      *resp->mutable_task_desc() = desc;
      // Mark task as RUNNING in PG when dispatched via heartbeat
      if (g_pg && g_pg->Connected()) {
        g_pg->UpdateTaskStatus(desc.task_id(), 1 /*RUNNING*/, "Agent开始执行采集");
      }
      LOG(INFO) << "Dispatched task " << desc.task_id() << " to agent " << ip;
    } else {
      resp->set_pending(false);
    }

    resp->set_status(drop::healthcheck::HealthCheckResponse::SERVING);
    return Status::OK;
  }
};

// ── HotmethodService ────────────────────────────────────────────────
class HotmethodServiceImpl final
    : public drop::hotmethod::Hotmethod::Service {
  Status Collect(ServerContext* /*ctx*/,
                 const drop::hotmethod::Target* /*req*/,
                 google::protobuf::Empty* /*resp*/) override {
    // Deprecated — tasks are dispatched via HealthCheck heartbeat now
    return Status(StatusCode::UNIMPLEMENTED, "use HealthCheck.Do for task dispatch");
  }

  Status NotifyResult(ServerContext* /*ctx*/,
                      const drop::hotmethod::TaskResult* req,
                      google::protobuf::Empty* /*resp*/) override {
    const std::string& tid = req->task_id();
    if (req->error_message().empty()) {
      // Success: transition RUNNING → UPLOADING → DONE
      if (g_pg && g_pg->Connected()) {
        g_pg->UpdateTaskStatus(tid, 2 /*UPLOADING*/, "采集完成，开始上传结果");
      }
      if (!req->cos_key().empty()) {
        if (g_pg && g_pg->Connected()) {
          g_pg->UpdateTaskStatus(tid, 3 /*DONE*/, "Agent上传完成，cos_key=" + req->cos_key());
        }
        LOG(INFO) << "Task " << tid << " DONE, cos_key=" << req->cos_key();
      } else if (req->file().name().empty()) {
        if (g_pg && g_pg->Connected()) {
          g_pg->UpdateTaskStatus(tid, 3 /*DONE*/, "Agent上报完成（无产出文件）");
        }
        LOG(INFO) << "Task " << tid << " DONE (no file)";
      } else {
        if (g_pg && g_pg->Connected()) {
          g_pg->UpdateTaskStatus(tid, 3 /*DONE*/, "Agent上报文件: " + req->file().name());
        }
        LOG(INFO) << "Task " << tid << " DONE with file " << req->file().name();
      }
    } else {
      // Failure
      if (g_pg && g_pg->Connected()) {
        g_pg->UpdateTaskStatus(tid, 4 /*FAILED*/, req->error_message());
      }
      LOG(WARNING) << "Task " << tid << " FAILED: " << req->error_message();
    }
    return Status::OK;
  }
};

// ── ControlService ──────────────────────────────────────────────────
class ControlServiceImpl final
    : public drop::control::Control::Service {
  Status CreateTask(ServerContext* /*ctx*/,
                    const drop::control::CreateTaskRequest* req,
                    drop::control::CreateTaskResponse* resp) override {
    const std::string& target_ip = req->target_ip();
    const drop::hotmethod::TaskDesc& desc = req->task_desc();

    // Push to in-memory queue
    g_task_queue.Push(target_ip, desc);
    LOG(INFO) << "Task " << desc.task_id() << " pushed to queue for " << target_ip
              << " (queue depth=" << g_task_queue.Size(target_ip) << ")";

    // PG status is already set to PENDING by apiserver before calling us
    resp->set_success(true);
    resp->set_message("task queued");
    return Status::OK;
  }

  Status FetchData(ServerContext* /*ctx*/,
                   const drop::control::FetchDataRequest* req,
                   drop::control::FetchDataResponse* resp) override {
    // For now return cos_key from PG if available
    if (g_pg && g_pg->Connected()) {
      // Simple query – in production we'd parameterize properly
      auto* c = static_cast<void*>(nullptr);  // Not needed for MVP
      resp->set_success(false);
      resp->set_error_message("FetchData not yet fully implemented - use MinIO presign");
    } else {
      resp->set_success(false);
      resp->set_error_message("no PG connection");
    }
    return Status::OK;
  }

  Status StatAgent(ServerContext* /*ctx*/,
                   const drop::control::StatAgentRequest* req,
                   drop::control::StatAgentResponse* resp) override {
    std::string filter_ip = req->target_ip();

    if (g_pg && g_pg->Connected()) {
      auto agents = g_pg->ListAgents();
      for (auto& a : agents) {
        if (!filter_ip.empty() && a.ip != filter_ip) continue;
        auto* info = resp->add_agents();
        info->set_host_name(a.hostname);
        info->set_ip_addr(a.ip);
        info->set_online(a.online);
        info->set_uid(a.uid);
        info->set_agent_version(a.version);
      }
    } else {
      // Fallback to in-memory tracker
      int timeout = FLAGS_hb_timeout;
      for (auto& [ip, online] : g_hb_tracker.AllAgents(timeout)) {
        if (!filter_ip.empty() && ip != filter_ip) continue;
        auto* info = resp->add_agents();
        info->set_ip_addr(ip);
        info->set_online(online);
      }
    }
    return Status::OK;
  }
};

// ── InitService ─────────────────────────────────────────────────────
class InitServiceImpl final
    : public drop::init::Init::Service {
  Status RegisterAgent(ServerContext* /*ctx*/,
                       const drop::init::RegisterAgentRequest* req,
                       drop::init::RegisterAgentResponse* resp) override {
    // Upsert into PG
    if (g_pg && g_pg->Connected()) {
      g_pg->UpsertAgent(req->ip_addr(), req->host_name(), req->uid(), req->agent_version());
    }
    resp->set_success(true);
    resp->set_message("registered");
    resp->set_uid(req->uid().empty() ? req->ip_addr() : req->uid());
    LOG(INFO) << "Agent registered: " << req->host_name() << " (" << req->ip_addr() << ")";
    return Status::OK;
  }

  Status FetchConfig(ServerContext* /*ctx*/,
                     const drop::init::FetchConfigRequest* /*req*/,
                     drop::init::FetchConfigResponse* resp) override {
    // Return default COS config (to be filled from environment later)
    auto* cfg = resp->mutable_cos_config();
    cfg->set_region("default");
    cfg->set_bucket("drop-data");
    resp->set_log_level("INFO");
    return Status::OK;
  }
};

// ── Heartbeat scanner thread ────────────────────────────────────────
static void HeartbeatScanner() {
  LOG(INFO) << "Heartbeat scanner started (interval="
            << FLAGS_hb_scan_interval << "s, timeout="
            << FLAGS_hb_timeout << "s)";
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(FLAGS_hb_scan_interval));
    if (g_pg && g_pg->Connected()) {
      int marked = g_pg->ScanAgentHeartbeats(FLAGS_hb_timeout);
      if (marked > 0) {
        LOG(INFO) << "Heartbeat scan: " << marked << " agent(s) marked offline";
      }
    }
  }
}

// ── main ────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // Connect to PG if DSN provided
  if (!FLAGS_pg_dsn.empty()) {
    g_pg = std::make_unique<drop::PGStore>(FLAGS_pg_dsn);
    if (!g_pg->Connected()) {
      LOG(WARNING) << "PG connection failed, running without PG persistence";
      g_pg.reset();
    }
  } else {
    LOG(WARNING) << "No --pg_dsn provided, running without PG persistence";
  }

  // Start heartbeat scanner thread
  std::thread hb_scanner(HeartbeatScanner);
  hb_scanner.detach();

  // Register services
  HealthCheckServiceImpl hc;
  HotmethodServiceImpl   hm;
  ControlServiceImpl     ctrl;
  InitServiceImpl        init_svc;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  std::string addr = std::string("0.0.0.0:") + std::to_string(FLAGS_port);
  ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&hc);
  builder.RegisterService(&hm);
  builder.RegisterService(&ctrl);
  builder.RegisterService(&init_svc);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  LOG(INFO) << "drop_server listening on " << addr;
  LOG(INFO) << "  pg_dsn            = " << (FLAGS_pg_dsn.empty() ? "(none)" : "***");
  LOG(INFO) << "  hb_timeout        = " << FLAGS_hb_timeout;
  LOG(INFO) << "  hb_scan_interval  = " << FLAGS_hb_scan_interval;

  server->Wait();
  return 0;
}
