/* #478: deterministic interleavings around the real kernel recovery functions.
 * Kernel APIs are mocked; PM/fence references, start/reset order, IRQ draining,
 * completion while stopping, and resubmission invariants are asserted.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define READ_ONCE(x) (x)
static void test_log(const char *format, ...) { }
#define DRM_WARN(...) test_log(__VA_ARGS__)
#define DRM_ERROR(...) test_log(__VA_ARGS__)
#define DRM_INFO(...) test_log(__VA_ARGS__)
#define spin_lock_irqsave(lock, flags) ((void)(flags = 0), (void)(lock))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define spin_lock(lock) ((void)(lock))
#define spin_unlock(lock) ((void)(lock))
#define system_highpri_wq NULL

enum drm_gpu_sched_stat { DRM_GPU_SCHED_STAT_RESET };
struct work_struct { bool pending; };
struct dma_fence { int refs; bool signaled; };
struct drm_gpu_scheduler { int unused; };
struct drm_sched_job { struct drm_gpu_scheduler *sched; };
struct lima_device { struct lima_sched_pipe *pipe; };
struct lima_ip { int irq; };
typedef uint64_t u64;
typedef uint64_t atomic64_t;
static uint64_t mock_time = 100;
static uint64_t ktime_get_ns(void) { return mock_time; }
static void atomic64_add(uint64_t value, atomic64_t *counter) { *counter += value; }
struct lima_vm { atomic64_t engine_ns[2]; };
struct lima_sched_task {
    struct drm_sched_job base;
    struct dma_fence *fence;
    struct lima_vm *vm;
    bool recoverable;
    u64 usage_start_ns;
};
struct lima_sched_pipe {
    struct drm_gpu_scheduler base;
    struct lima_device *ldev;
    struct lima_ip *processor[2], *bcast_processor, *mmu[2], *bcast_mmu;
    int num_processor, num_mmu, hw_lock;
    struct lima_sched_task *current_task, *next_task;
    struct lima_vm *current_vm;
    struct work_struct start_work, recover_work;
    bool error, recovering;
    void (*task_fini)(struct lima_sched_pipe *);
    void (*task_error)(struct lima_sched_pipe *);
    void (*task_mask_irq)(struct lima_sched_pipe *);
};

static int lima_max_error_tasks, lima_irq_start;
static int pm_refs, pm_puts, starts, resets, cancelled_starts, cancelled_recover;
static int resubmits, resumes, syncs;
static bool stopped, reset_started, worker_wins, complete_at_stop, late_enqueue, latency_completion;
static struct lima_sched_pipe pipe_states[2];
#define pipe_state pipe_states[0]
static struct lima_sched_task a, b;
static struct dma_fence fa, fb;
static struct lima_device device;
static struct lima_ip ip[4];
static struct lima_vm vm;

static void lima_sched_start_work(struct work_struct *);
void lima_sched_pipe_task_done(struct lima_sched_pipe *);
static struct lima_sched_task *to_lima_task(struct drm_sched_job *job) {
    return container_of(job, struct lima_sched_task, base);
}
static struct lima_sched_pipe *to_lima_pipe(struct drm_gpu_scheduler *sched) {
    return container_of(sched, struct lima_sched_pipe, base);
}
static bool dma_fence_is_signaled(struct dma_fence *fence) { assert(fence); return fence->signaled; }
static void dma_fence_signal(struct dma_fence *fence) { assert(fence && !fence->signaled); fence->signaled = true; }
static void dma_fence_put(struct dma_fence *fence) { assert(fence && fence->refs > 0); fence->refs--; }
static void lima_pm_idle(struct lima_device *dev) { assert(pm_refs > 0); pm_refs--; pm_puts++; }
static void lima_vm_put(struct lima_vm *old) { }
static const char *lima_ip_name(struct lima_ip *processor) { return "mock"; }
static bool lima_sched_vm_is_loaded(struct lima_sched_pipe *pipe, struct lima_vm *v) { return true; }
static struct lima_vm *lima_sched_hw_start(struct lima_sched_pipe *pipe, struct lima_sched_task *task) {
    assert(!pipe->recovering && !reset_started);
    starts++;
    task->usage_start_ns = ktime_get_ns();
    pipe->current_task = task;
    return NULL;
}
static void synchronize_irq(int irq) {
    syncs++;
    if (latency_completion && !stopped) {
        latency_completion = false;
        lima_sched_pipe_task_done(&pipe_state);
    }
    if (late_enqueue && pipe_state.recovering) {
        /* An IRQ passed its guard before recovery, then queued after cancel. */
        late_enqueue = false;
        pipe_state.start_work.pending = true;
    }
}
static void cancel_work_sync(struct work_struct *work) {
    if (work == &pipe_state.start_work) {
        cancelled_starts++;
        if (work->pending) {
            /* Model a worker already dispatched when cancellation begins. */
            lima_sched_start_work(work);
            work->pending = false;
        }
    } else {
        assert(work == &pipe_state.recover_work && !reset_started);
        cancelled_recover++;
        work->pending = false;
    }
}
static void queue_work(void *queue, struct work_struct *work) { work->pending = true; }
static void schedule_work(struct work_struct *work) { work->pending = true; }
static void drm_sched_fault(struct drm_gpu_scheduler *sched) { assert(!reset_started); }
static void drm_sched_stop(struct drm_gpu_scheduler *sched, struct drm_sched_job *bad) {
    stopped = true;
    if (worker_wins) {
        pipe_state.start_work.pending = false;
        lima_sched_start_work(&pipe_state.start_work);
    }
    if (complete_at_stop)
        lima_sched_pipe_task_done(&pipe_state);
}
static void drm_sched_increase_karma(struct drm_sched_job *task) { assert(stopped); }
static void lima_sched_build_error_task_list(struct lima_sched_task *task) { }
static void lima_mmu_page_fault_resume(struct lima_ip *mmu) { assert(reset_started); resumes++; }
static void drm_sched_resubmit_jobs(struct drm_gpu_scheduler *sched) {
    assert(stopped && !pipe_state.recovering);
    assert(!pipe_state.current_task && !pipe_state.next_task && pm_refs == 0);
    assert(!pipe_state.start_work.pending && !pipe_state.recover_work.pending);
    resubmits++;
}
static void drm_sched_start(struct drm_gpu_scheduler *sched, int error) { assert(resubmits == 1); }
static void task_fini(struct lima_sched_pipe *pipe) { }
static void task_mask_irq(struct lima_sched_pipe *pipe) { }
static void task_error(struct lima_sched_pipe *pipe) {
    assert(cancelled_starts && cancelled_recover && syncs >= 4);
    assert(stopped && pipe->recovering);
    assert(!pipe->start_work.pending && !pipe->recover_work.pending);
    reset_started = true;
    resets++;
    /* Neither a late completion nor deferred work may take task ownership. */
    int refs = pm_refs, count = starts;
    lima_sched_start_work(&pipe->start_work);
    lima_sched_pipe_task_done(pipe);
    assert(pm_refs == refs && starts == count);
}

/* PATCHED_FUNCTIONS */

static void setup(bool running, bool queued) {
    memset(&pipe_state, 0, sizeof(pipe_state));
    pm_refs = running + queued;
    pm_puts = starts = resets = cancelled_starts = cancelled_recover = resubmits = resumes = syncs = 0;
    stopped = reset_started = worker_wins = complete_at_stop = late_enqueue = latency_completion = false;
    fa = (struct dma_fence) { .refs = 1 };
    fb = (struct dma_fence) { .refs = 1 };
    a = (struct lima_sched_task) { .base.sched = &pipe_state.base, .fence = &fa, .vm = &vm };
    b = (struct lima_sched_task) { .base.sched = &pipe_state.base, .fence = &fb, .vm = &vm };
    memset(&vm, 0, sizeof(vm));
    device.pipe = &pipe_state;
    mock_time = 100;
    a.usage_start_ns = running ? 10 : 0;
    pipe_state.ldev = &device;
    pipe_state.current_vm = &vm;
    pipe_state.current_task = running ? &a : NULL;
    pipe_state.next_task = queued ? &b : NULL;
    pipe_state.start_work.pending = queued && !running;
    pipe_state.num_processor = pipe_state.num_mmu = 2;
    for (int i = 0; i < 2; i++) {
        pipe_state.processor[i] = &ip[i];
        pipe_state.mmu[i] = &ip[i + 2];
    }
    pipe_state.task_fini = task_fini;
    pipe_state.task_mask_irq = task_mask_irq;
    pipe_state.task_error = task_error;
}

int main(void) {
    setup(true, false);
    lima_sched_timedout_job(&a.base);
    assert(resets == 1 && pm_puts == 1 && fa.refs == 0 && !a.fence);
    assert(vm.engine_ns[0] == 90 && !a.usage_start_ns && !b.usage_start_ns);

    setup(true, true);
    pipe_state.recover_work.pending = true;
    lima_sched_timedout_job(&a.base);
    assert(resets == 1 && pm_puts == 2 && fa.refs == 0 && fb.refs == 0);
    assert(vm.engine_ns[0] == 90 && !a.usage_start_ns && !b.usage_start_ns);

    setup(false, true);  /* Reviewer's deferred-only timeout, task == next. */
    lima_sched_timedout_job(&b.base);
    assert(resets == 1 && pm_puts == 1 && fb.refs == 0 && !b.fence && starts == 0);

    setup(false, true);  /* Worker started before the recovery guard. */
    worker_wins = true;
    lima_sched_timedout_job(&b.base);
    assert(resets == 1 && pm_puts == 1 && fb.refs == 0 && starts == 1);

    setup(false, true);  /* IRQ queues start_work after the first cancellation. */
    late_enqueue = true;
    lima_sched_timedout_job(&b.base);
    assert(cancelled_starts == 2 && starts == 0 && pm_puts == 1 && fb.refs == 0);

    setup(true, true);  /* Completion during drm_sched_stop leaves only next. */
    complete_at_stop = true;
    lima_sched_timedout_job(&a.base);
    assert(resets == 1 && pm_puts == 2 && fa.signaled && fa.refs == 1 && fb.refs == 0);

    setup(true, false);  /* Completion while stopping: no outstanding owner. */
    complete_at_stop = true;
    lima_sched_timedout_job(&a.base);
    assert(resets == 0 && pm_puts == 1 && fa.signaled && fa.refs == 1);

    setup(true, false);  /* High interrupt latency: early exit, no reset. */
    latency_completion = true;
    lima_sched_timedout_job(&a.base);
    assert(!stopped && resets == 0 && pm_puts == 1 && fa.signaled);

    setup(false, false); /* Already signaled: no PM reference to release. */
    fa.signaled = true;
    lima_sched_timedout_job(&a.base);
    assert(!stopped && resets == 0 && pm_puts == 0);

    setup(true, false);
    mock_time = 110;
    lima_sched_pipe_task_done(&pipe_state);
    assert(vm.engine_ns[0] == 100 && vm.engine_ns[1] == 0);
    lima_sched_account_done(&pipe_state, &a);  // no duplicate completion charge
    assert(vm.engine_ns[0] == 100);
    mock_time = 500;
    assert(vm.engine_ns[0] == 100);  // idle: no counter read writes a timestamp
    pipe_states[1].ldev = &device;
    b.usage_start_ns = 200;
    lima_sched_account_done(&pipe_states[1], &b);
    assert(vm.engine_ns[0] == 100 && vm.engine_ns[1] == 300 && !b.usage_start_ns);

    puts("lima-timeout-sim: PASS (9 recovery interleavings + GP/PP accounting, ASan/UBSan)");
    return 0;
}
