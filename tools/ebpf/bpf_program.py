BPF_PROGRAM = r"""
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

struct event_t {
    u32 pid;
    u64 ts_ns;
    u64 addr;
    u64 size;
    u32 action;

    /*
     * get_stackid() 失败时会返回负数错误码，
     * 因此这里必须使用有符号 int。
     */
    int user_stack_id;
    int kernel_stack_id;

    char comm[TASK_COMM_LEN];
};

BPF_PERF_OUTPUT(events);

/*
 * 保存调用栈地址。
 *
 * 4096 表示最多保存 4096 个不同的调用栈。
 * 每个调用栈由多个地址组成。
 */
BPF_STACK_TRACE(stack_traces, 4096);

int trace_alloc_return(struct pt_regs *ctx)
{
    struct event_t event = {};

    u64 addr = PT_REGS_RC(ctx);

    /*
     * 过滤失败分配：
     *   addr == 0        —— 空指针
     *   addr >= (u64)-4095 —— ERR_PTR 形式的负错误码（-1 ~ -4095），
     *                        如 ERR_PTR(-ENOMEM)，内核错误码均落在此区间。
     */
    if (addr == 0 || addr >= (u64)-4095) {
        return 0;
    }

    event.pid = bpf_get_current_pid_tgid() >> 32;
    event.ts_ns = bpf_ktime_get_ns();
    event.addr = addr;
    event.size = 2 * 1024 * 1024;
    event.action = 1;

    event.user_stack_id =
        stack_traces.get_stackid(ctx, BPF_F_USER_STACK);

    event.kernel_stack_id =
        stack_traces.get_stackid(ctx, 0);

    bpf_get_current_comm(&event.comm, sizeof(event.comm));

    events.perf_submit(ctx, &event, sizeof(event));
    return 0;
}

int trace_free_entry(struct pt_regs *ctx)
{
    struct event_t event = {};

    event.pid = bpf_get_current_pid_tgid() >> 32;
    event.ts_ns = bpf_ktime_get_ns();
    event.addr = PT_REGS_PARM1(ctx);
    event.size = 2 * 1024 * 1024;
    event.action = 2;

    /*
     * 当前主要关注申请来源，释放事件暂时不采集调用栈。
     */
    event.user_stack_id = -1;
    event.kernel_stack_id = -1;

    bpf_get_current_comm(&event.comm, sizeof(event.comm));

    events.perf_submit(ctx, &event, sizeof(event));
    return 0;
}
"""