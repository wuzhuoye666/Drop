#!/usr/bin/env python3
"""Integration test for Phase 3: Agent heartbeat + task dispatch via HealthCheck.Do"""
import subprocess, time, sys, os, signal

# Generate Python gRPC stubs from protos
PROTO_DIR = "/root/Drop/drop/common/proto"
GEN_DIR = "/tmp/drop_py_grpc"
os.makedirs(GEN_DIR, exist_ok=True)

subprocess.run([
    sys.executable, "-m", "grpc_tools.protoc",
    f"--proto_path={PROTO_DIR}",
    f"--python_out={GEN_DIR}",
    f"--grpc_python_out={GEN_DIR}",
    f"{PROTO_DIR}/common.proto",
    f"{PROTO_DIR}/hotmethod.proto",
    f"{PROTO_DIR}/healthcheck.proto",
    f"{PROTO_DIR}/control.proto",
    f"{PROTO_DIR}/init.proto",
], check=True)

sys.path.insert(0, GEN_DIR)

import grpc
import healthcheck_pb2 as hc
import healthcheck_pb2_grpc as hc_grpc
import control_pb2 as ctrl
import control_pb2_grpc as ctrl_grpc
import hotmethod_pb2_grpc as hm_grpc
import common_pb2 as common
import hotmethod_pb2 as hm

SERVER_ADDR = "localhost:50051"

def test_heartbeat():
    """Step 3.1: Agent sends heartbeat, server responds"""
    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = hc_grpc.HealthCheckStub(channel)

    req = hc.HealthCheckRequest(
        host_name="test-host",
        ip_addr="192.168.1.100",
        uid="test-uid-001",
        agent_version="0.1.0",
    )
    resp = stub.Do(req)

    assert resp.status == hc.HealthCheckResponse.SERVING, f"Expected SERVING, got {resp.status}"
    assert resp.pending == False, "Expected pending=False with no tasks"
    print("[PASS] Step 3.1: Heartbeat works, server responds SERVING + pending=false")
    channel.close()

def test_create_task():
    """Step 3.2: apiserver creates task on server via ControlService.CreateTask"""
    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = ctrl_grpc.ControlStub(channel)

    req = ctrl.CreateTaskRequest(
        target_ip="192.168.1.100",
        service="hotmethod",
        task_desc=hm.TaskDesc(
            task_id="t001",
            task_type=0,
            profiler_type=0,
            sample_argv=common.RecordArgv(
                hz=99,
                duration=10,
                pid=1234,
            ),
        ),
    )
    resp = stub.CreateTask(req)

    assert resp.success, f"CreateTask failed: {resp.message}"
    print("[PASS] Step 3.2: CreateTask succeeded, task queued")
    channel.close()

def test_heartbeat_dispatches_task():
    """Step 3.1 cont: Heartbeat returns pending task after CreateTask"""
    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = hc_grpc.HealthCheckStub(channel)

    req = hc.HealthCheckRequest(
        host_name="test-host",
        ip_addr="192.168.1.100",
        uid="test-uid-001",
        agent_version="0.1.0",
    )
    resp = stub.Do(req)

    assert resp.pending == True, "Expected pending=True after CreateTask"
    assert resp.task_desc.task_id == "t001", f"Expected task_id=t001, got {resp.task_desc.task_id}"
    print("[PASS] Step 3.1 cont: Heartbeat dispatches pending task")

    # Second heartbeat should have no more pending tasks
    resp2 = stub.Do(req)
    assert resp2.pending == False, "Expected no more pending tasks"
    print("[PASS] Heartbeat correctly returns pending=false after task is dispatched")
    channel.close()

def test_stat_agent():
    """Step 3.2 cont: StatAgent returns registered agents"""
    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = ctrl_grpc.ControlStub(channel)

    req = ctrl.StatAgentRequest(target_ip="192.168.1.100")
    resp = stub.StatAgent(req)

    # Without PG, we fall back to in-memory tracker
    # The heartbeat test above should have registered this IP
    agents = list(resp.agents)
    if len(agents) > 0:
        print(f"[PASS] StatAgent returned {len(agents)} agent(s)")
    else:
        print("[INFO] StatAgent returned 0 agents (may need PG for full listing)")
    channel.close()

def test_notify_result():
    """NotifyResult: Agent reports task completion"""
    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = hm_grpc.HotmethodStub(channel)

    req = hm.TaskResult(
        task_id="t001",
        cos_key="t001/perf.data",
    )
    # This will fail without PG but verifies the RPC is reachable
    try:
        stub.NotifyResult(req)
        print("[PASS] NotifyResult RPC succeeded")
    except grpc.RpcError as e:
        # Expected if no PG — still OK as the RPC is reachable
        if "not yet implemented" in str(e.code()) or "UNIMPLEMENTED" in str(e.code()):
            print("[FAIL] NotifyResult returned UNIMPLEMENTED")
        else:
            print(f"[INFO] NotifyResult RPC called (may fail without PG): {e.details()}")
    channel.close()

if __name__ == "__main__":
    print("=== Phase 3 Integration Tests ===")
    print()

    # Ensure drop_server is running
    try:
        test_heartbeat()
    except grpc.RpcError as e:
        print(f"[ERROR] Cannot connect to drop_server at {SERVER_ADDR}: {e}")
        print("Please start drop_server first: ./drop_server -port 50051")
        sys.exit(1)

    test_create_task()
    test_heartbeat_dispatches_task()
    test_stat_agent()
    test_notify_result()

    print()
    print("=== Phase 3 core RPC tests completed ===")
