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
    // Read filter PID from map. If map entry missing → system-wide (no filter).
    u32 key = 0;
    u32 *filter_ptr = bpf_map_lookup_elem(&target_pid, &key);
    u32 filter_tgid = filter_ptr ? *filter_ptr : 0;

    // Get prev task's tgid (thread group ID = process PID)
    struct task_struct *prev = (struct task_struct *)bpf_get_current_task();
    u32 prev_tgid = 0;
    u32 prev_pid = 0;
    bpf_probe_read_kernel(&prev_tgid, sizeof(prev_tgid), &prev->tgid);
    bpf_probe_read_kernel(&prev_pid, sizeof(prev_pid), &prev->pid);

    // Apply PID filter: if filter_tgid > 0, only trace that process and its threads
    if (filter_tgid != 0 && prev_tgid != filter_tgid)
        return 0;

    struct stack_key sk = {};
    sk.pid = prev_pid;
    sk.tgid = prev_tgid;
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
