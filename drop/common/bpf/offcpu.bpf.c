// offcpu.bpf.c — Off-CPU profiling eBPF kernel probe
// Captures scheduler switch events to build off-CPU flame graphs.
// Tracepoint: sched/sched_switch
// Maps: stack_traces (BPF_MAP_TYPE_STACK_TRACE), counts (HASH)
//
// target_pid map: key=0, value=PID to filter (0 = system-wide).

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define MAX_ENTRIES 10240
#define TASK_COMM_LEN 16

struct stack_key {
    u32 pid;
    u32 tgid;
    int user_stack_id;
    int kern_stack_id;
};

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, MAX_ENTRIES);
    __type(key, u32);
    __type(value, u64);
} stack_traces SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_ENTRIES);
    __type(key, struct stack_key);
    __type(value, u64);
} counts SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} target_pid SEC(".maps");

SEC("tracepoint/sched/sched_switch")
int trace_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    // Read filter PID from map. If map entry missing -> system-wide (no filter).
    u32 key = 0;
    u32 *filter_ptr = bpf_map_lookup_elem(&target_pid, &key);
    u32 filter_tgid = filter_ptr ? *filter_ptr : 0;

    // IMPORTANT: bpf_get_current_task() returns the NEXT task being scheduled
    // in, NOT the prev task going off-CPU. Use ctx->prev_pid from the
    // tracepoint context which correctly identifies the task leaving the CPU.
    u32 prev_pid = ctx->prev_pid;

    // For TGID: we only have prev_pid from the tracepoint, not prev_tgid.
    // For single-threaded processes (the common case), tgid == pid.
    // For multi-threaded: use filter_tgid if set, otherwise prev_pid.
    // Userspace can resolve thread→process mapping via /proc if needed.

    // Apply PID filter: if filter_tgid > 0, only trace threads of that process.
    // Since we only have prev_pid (not tgid) in the tracepoint, we check:
    // - Exact PID match (covers single-threaded processes and process leaders)
    // For eBPF profiling, filtering by PID covers the vast majority of use cases.
    if (filter_tgid != 0 && prev_pid != filter_tgid) {
        // We cannot efficiently check thread membership in BPF,
        // so when filtering by PID, we only capture the main thread.
        // Users can set pid=0 for system-wide profiling.
        return 0;
    }

    struct stack_key sk = {};
    sk.pid = prev_pid;
    sk.tgid = filter_tgid != 0 ? filter_tgid : prev_pid;
    sk.user_stack_id = bpf_get_stackid(ctx, &stack_traces, BPF_F_USER_STACK);
    sk.kern_stack_id = bpf_get_stackid(ctx, &stack_traces, 0);

    u64 *val = bpf_map_lookup_elem(&counts, &sk);
    if (val)
        __sync_fetch_and_add(val, 1);
    else {
        u64 one = 1;
        bpf_map_update_elem(&counts, &sk, &one, BPF_ANY);
    }

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
