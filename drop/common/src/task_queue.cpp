#include "task_queue.h"

#include <libpq-fe.h>
#include <glog/logging.h>

#include <cstring>
#include <sstream>

namespace drop {

PGStore::PGStore(const std::string& conninfo) : conn_(nullptr) {
  PGconn* c = PQconnectdb(conninfo.c_str());
  if (PQstatus(c) != CONNECTION_OK) {
    LOG(ERROR) << "PG connect failed: " << PQerrorMessage(c);
    PQfinish(c);
    return;
  }
  conn_ = c;
  LOG(INFO) << "PGStore connected";
}

PGStore::~PGStore() {
  if (conn_) {
    PQfinish(static_cast<PGconn*>(conn_));
  }
}

void PGStore::UpsertAgent(const std::string& ip, const std::string& hostname,
                           const std::string& uid, const std::string& version) {
  if (!conn_) return;
  auto* c = static_cast<PGconn*>(conn_);

  // First check if agent was offline before upsert (for audit log)
  bool was_offline = false;
  std::string agent_id;
  {
    const char* sel_sql = "SELECT id, online FROM agent_info WHERE ip_addr = $1";
    const char* sel_params[1] = {ip.c_str()};
    PGresult* sel_res = PQexecParams(c, sel_sql, 1, nullptr, sel_params, nullptr, nullptr, 0);
    if (PQresultStatus(sel_res) == PGRES_TUPLES_OK && PQntuples(sel_res) > 0) {
      agent_id = PQgetvalue(sel_res, 0, 0);
      std::string online_str = PQgetvalue(sel_res, 0, 1);
      was_offline = (online_str == "f");
    }
    PQclear(sel_res);
  }

  // Upsert: insert on conflict (ip_addr) do update
  const char* sql =
      "INSERT INTO agent_info (hostname, ip_addr, online, uid, version, last_hb) "
      "VALUES ($1, $2, true, $3, $4, NOW()) "
      "ON CONFLICT (ip_addr) DO UPDATE SET "
      "  hostname = EXCLUDED.hostname, "
      "  online = true, "
      "  uid = EXCLUDED.uid, "
      "  version = EXCLUDED.version, "
      "  last_hb = NOW()";

  const char* params[4] = {hostname.c_str(), ip.c_str(), uid.c_str(), version.c_str()};
  PGresult* res = PQexecParams(c, sql, 4, nullptr, params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "UpsertAgent failed: " << PQerrorMessage(c);
  }
  PQclear(res);

  // If agent was offline and now came back online, write audit log
  if (was_offline && !agent_id.empty()) {
    // Get the agent_id if we didn't have it (new insert case)
    if (agent_id.empty()) {
      const char* id_sql = "SELECT id FROM agent_info WHERE ip_addr = $1";
      const char* id_params[1] = {ip.c_str()};
      PGresult* id_res = PQexecParams(c, id_sql, 1, nullptr, id_params, nullptr, nullptr, 0);
      if (PQresultStatus(id_res) == PGRES_TUPLES_OK && PQntuples(id_res) > 0) {
        agent_id = PQgetvalue(id_res, 0, 0);
      }
      PQclear(id_res);
    }

    const char* audit_sql =
        "INSERT INTO agent_audit_log (agent_id, event_type, reason, timestamp) "
        "VALUES ($1, 'online', 'heartbeat recovered', NOW())";
    const char* audit_params[1] = {agent_id.c_str()};
    PGresult* audit_res = PQexecParams(c, audit_sql, 1, nullptr, audit_params, nullptr, nullptr, 0);
    PQclear(audit_res);
  }
}

void PGStore::UpdateTaskStatus(const std::string& tid, int new_status, const std::string& reason) {
  if (!conn_) return;
  auto* c = static_cast<PGconn*>(conn_);

  // Begin transaction
  PGresult* res = PQexec(c, "BEGIN");
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "BEGIN failed: " << PQerrorMessage(c);
    PQclear(res);
    return;
  }
  PQclear(res);

  // SELECT FOR UPDATE
  const char* sel_sql = "SELECT status FROM hotmethod_task WHERE tid = $1 FOR UPDATE";
  const char* sel_params[1] = {tid.c_str()};
  res = PQexecParams(c, sel_sql, 1, nullptr, sel_params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
    LOG(ERROR) << "SELECT FOR UPDATE failed for tid=" << tid;
    PQclear(res);
    PQexec(c, "ROLLBACK");
    return;
  }
  int old_status = std::stoi(PQgetvalue(res, 0, 0));
  PQclear(res);

  // Validate transition (mirror Go state machine)
  bool allowed = IsTransitionAllowed(old_status, new_status);

  if (!allowed) {
    LOG(WARNING) << "Illegal transition " << old_status << "->" << new_status
                 << " for task " << tid;
    PQexec(c, "ROLLBACK");
    return;
  }

  // UPDATE
  static const int kRunning = 1, kDone = 3, kFailed = 4;
  std::string update_sql =
      "UPDATE hotmethod_task SET status = $1, status_info = $2";
  if (new_status == kRunning) update_sql += ", begin_time = NOW()";
  if (new_status == kDone || new_status == kFailed) update_sql += ", end_time = NOW()";
  update_sql += " WHERE tid = $3";

  std::string status_str = std::to_string(new_status);
  const char* upd_params[3] = {status_str.c_str(), reason.c_str(), tid.c_str()};
  res = PQexecParams(c, update_sql.c_str(), 3, nullptr, upd_params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "UPDATE task failed: " << PQerrorMessage(c);
    PQclear(res);
    PQexec(c, "ROLLBACK");
    return;
  }
  PQclear(res);

  // INSERT task_status_history
  const char* hist_sql =
      "INSERT INTO task_status_history (tid, old_status, new_status, reason, timestamp) "
      "VALUES ($1, $2, $3, $4, NOW())";
  std::string old_str = std::to_string(old_status);
  const char* hist_params[4] = {tid.c_str(), old_str.c_str(), status_str.c_str(), reason.c_str()};
  res = PQexecParams(c, hist_sql, 4, nullptr, hist_params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "INSERT history failed: " << PQerrorMessage(c);
    PQclear(res);
    PQexec(c, "ROLLBACK");
    return;
  }
  PQclear(res);

  // COMMIT
  res = PQexec(c, "COMMIT");
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "COMMIT failed: " << PQerrorMessage(c);
  }
  PQclear(res);

  LOG(INFO) << "Task " << tid << ": " << old_status << "->" << new_status
            << " reason=" << reason;
}

int PGStore::ScanAgentHeartbeats(int timeout_sec) {
  if (!conn_) return 0;
  auto* c = static_cast<PGconn*>(conn_);

  // Find agents marked online but stale
  std::string sql =
      "SELECT id, ip_addr FROM agent_info "
      "WHERE online = true AND last_hb < NOW() - INTERVAL '"
      + std::to_string(timeout_sec) + " seconds'";

  PGresult* res = PQexec(c, sql.c_str());
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    LOG(ERROR) << "ScanAgentHeartbeats query failed: " << PQerrorMessage(c);
    PQclear(res);
    return 0;
  }

  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    std::string agent_id = PQgetvalue(res, i, 0);
    std::string ip = PQgetvalue(res, i, 1);

    // Mark offline
    const char* upd_sql = "UPDATE agent_info SET online = false WHERE id = $1";
    const char* upd_params[1] = {agent_id.c_str()};
    PGresult* r2 = PQexecParams(c, upd_sql, 1, nullptr, upd_params, nullptr, nullptr, 0);
    PQclear(r2);

    // Audit log
    const char* audit_sql =
        "INSERT INTO agent_audit_log (agent_id, event_type, reason, timestamp) "
        "VALUES ($1, 'offline', $2, NOW())";
    std::string reason = std::to_string(timeout_sec) + "s\u65e0\u5fc3\u8df3";  // "s无心跳"
    const char* audit_params[2] = {agent_id.c_str(), reason.c_str()};
    r2 = PQexecParams(c, audit_sql, 2, nullptr, audit_params, nullptr, nullptr, 0);
    PQclear(r2);

    LOG(INFO) << "Agent " << ip << " (id=" << agent_id << ") marked offline";
  }
  PQclear(res);

  // Also detect agents that came back online
  {
    std::string sql2 =
        "SELECT id, ip_addr FROM agent_info "
        "WHERE online = false AND last_hb >= NOW() - INTERVAL '"
        + std::to_string(timeout_sec) + " seconds'";
    PGresult* res2 = PQexec(c, sql2.c_str());
    if (PQresultStatus(res2) == PGRES_TUPLES_OK) {
      int n2 = PQntuples(res2);
      for (int i = 0; i < n2; ++i) {
        std::string agent_id = PQgetvalue(res2, i, 0);
        std::string ip = PQgetvalue(res2, i, 1);

        const char* upd_sql = "UPDATE agent_info SET online = true WHERE id = $1";
        const char* upd_params[1] = {agent_id.c_str()};
        PGresult* r2 = PQexecParams(c, upd_sql, 1, nullptr, upd_params, nullptr, nullptr, 0);
        PQclear(r2);

        const char* audit_sql =
            "INSERT INTO agent_audit_log (agent_id, event_type, reason, timestamp) "
            "VALUES ($1, 'online', 'heartbeat recovered', NOW())";
        const char* audit_params[1] = {agent_id.c_str()};
        r2 = PQexecParams(c, audit_sql, 1, nullptr, audit_params, nullptr, nullptr, 0);
        PQclear(r2);

        LOG(INFO) << "Agent " << ip << " (id=" << agent_id << ") back online";
      }
    }
    PQclear(res2);
  }

  return nrows;
}

std::vector<PGStore::AgentRecord> PGStore::ListAgents() {
  std::vector<AgentRecord> result;
  if (!conn_) return result;
  auto* c = static_cast<PGconn*>(conn_);

  const char* sql = "SELECT hostname, ip_addr, online, uid, version FROM agent_info ORDER BY id";
  PGresult* res = PQexec(c, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    LOG(ERROR) << "ListAgents failed: " << PQerrorMessage(c);
    PQclear(res);
    return result;
  }

  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    AgentRecord rec;
    rec.hostname = PQgetvalue(res, i, 0);
    rec.ip       = PQgetvalue(res, i, 1);
    rec.online   = (std::string(PQgetvalue(res, i, 2)) == "t");
    rec.uid      = PQgetvalue(res, i, 3);
    rec.version  = PQgetvalue(res, i, 4);
    result.push_back(std::move(rec));
  }
  PQclear(res);
  return result;
}

bool IsTransitionAllowed(int old_status, int new_status) {
  if (old_status == new_status) return false;
  static const int kPending = 0, kRunning = 1, kUploading = 2, kDone = 3, kFailed = 4;
  if (old_status == kPending && (new_status == kRunning || new_status == kFailed)) return true;
  if (old_status == kRunning && (new_status == kUploading || new_status == kFailed)) return true;
  if (old_status == kUploading && (new_status == kDone || new_status == kFailed)) return true;
  if (old_status == kFailed && new_status == kPending) return true;
  return false;
}

}  // namespace drop
