#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual queue/remove/release callbacks against controlled host shims.

The worker model exposes the mem2mem-core window after TRANS_RUNNING is cleared
but before job_finish stops accessing its context. No sleeps or probabilistic
race loops: condition variables place a competing callback at the device mutex.
"""
from pathlib import Path
import sys
import re

src = Path(sys.argv[1]).read_text()

def function(name):
    pos = re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', src).start()
    start = src.rfind('static ', 0, pos)
    end = src.index('{', pos) + 1
    depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end] + '\n'

out = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* vb2's zero-size fallback is the embedded vb, not the trailing m2m list. */
struct vb2_v4l2_buffer { uintptr_t fields[8]; };
struct list_head { struct list_head *next, *prev; };
struct v4l2_m2m_buffer { struct vb2_v4l2_buffer vb; struct list_head list; };
struct mutex { pthread_mutex_t raw; };
struct vb2_queue {
    unsigned type, io_modes, timestamp_flags, buf_struct_size;
    bool bidirectional;
    void *drv_priv, *dev;
    const void *ops, *mem_ops;
    struct mutex *lock;
};
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
#define PAGE_SIZE 4096
#define VB2_MMAP 1
#define VB2_DMABUF 2
#define V4L2_BUF_FLAG_TIMESTAMP_COPY 0x4000
static int rga1_qops, vb2_dma_sg_memops;
struct v4l2_device { int stub; };
struct rga1_dev {
    struct mutex mutex;
    bool disconnected;
    void *dev, *m2m, *wq;
    int irq, video;
    struct v4l2_device v4l2;
};
struct model_ctx { unsigned marker; };
struct rga1_ctx {
    struct rga1_dev *rga;
    struct { struct model_ctx *m2m_ctx; } fh;
    int ctrls;
    void *scratch,*expanded;
    void *page_table, *guard, *aux;
    size_t page_table_size;
    uintptr_t page_table_dma, guard_dma, aux_dma;
    size_t scratch_size,expanded_size;
    uintptr_t scratch_dma,expanded_dma;
};
struct file { struct rga1_ctx *ctx; };
static void rga1_put_mask(struct rga1_ctx *ctx) {(void)ctx;}
struct platform_device { struct rga1_dev *rga; };
static struct rga1_ctx *rga1_ctx(struct file *f) { return f->ctx; }
static struct rga1_dev *platform_get_drvdata(struct platform_device *p) { return p->rga; }

/* Each access to gate state is serialized, independently of the device lock. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event = PTHREAD_COND_INITIALIZER;
enum actor { NONE, REMOVE, CLOSE };
static _Thread_local enum actor actor;
static _Thread_local bool holds_device_mutex;
static enum actor first;
static bool worker_at_tail, allow_tail, worker_done, first_flush;
static bool contender_attempted, contender_blocked, destroyed, paused, freed;
static unsigned init_count, freed_count;
static struct model_ctx *worker_ctx;

static void mutex_lock(struct mutex *m)
{
    int ret = pthread_mutex_trylock(&m->raw);
    assert(ret == 0 || ret == EBUSY);
    if (actor != first) {
        pthread_mutex_lock(&gate);
        contender_blocked = ret == EBUSY;
        contender_attempted = true;
        pthread_cond_broadcast(&event);
        pthread_mutex_unlock(&gate);
    }
    if (ret == EBUSY) assert(!pthread_mutex_lock(&m->raw));
    holds_device_mutex = true;
}
static void mutex_unlock(struct mutex *m)
{
    assert(holds_device_mutex);
    holds_device_mutex = false;
    assert(!pthread_mutex_unlock(&m->raw));
}
static void v4l2_m2m_suspend(void *m)
{
    /* Pausing outside the device lock can be undone by a closing context. */
    assert(holds_device_mutex);
    pthread_mutex_lock(&gate);
    paused = true;
    assert(worker_at_tail); /* TRANS_RUNNING is already clear; no worker wait. */
    pthread_mutex_unlock(&gate);
}
static void v4l2_m2m_resume(void *m)
{
    assert(holds_device_mutex);
    pthread_mutex_lock(&gate);
    assert(worker_done && !destroyed);
    paused = false;
    pthread_mutex_unlock(&gate);
}
static void flush_workqueue(void *wq)
{
    pthread_mutex_lock(&gate);
    assert(!destroyed);
    if (actor == first) {
        first_flush = true;
        pthread_cond_broadcast(&event);
    }
    while (!worker_done) pthread_cond_wait(&event, &gate);
    pthread_mutex_unlock(&gate);
}
static void destroy_workqueue(void *wq)
{
    pthread_mutex_lock(&gate);
    assert(worker_done && paused);
    destroyed = true;
    pthread_mutex_unlock(&gate);
}
static void v4l2_m2m_ctx_release(struct model_ctx *ctx)
{
    pthread_mutex_lock(&gate);
    assert(worker_done && !freed);
    freed = true;
    freed_count++;
    pthread_mutex_unlock(&gate);
    free(ctx);
}
static void v4l2_device_disconnect(struct v4l2_device *v)
{
    assert(holds_device_mutex);
    pthread_mutex_lock(&gate);
    assert(worker_done && paused);
    pthread_mutex_unlock(&gate);
}
static void video_unregister_device(int *v) { assert(holds_device_mutex); }
static void v4l2_device_get(struct v4l2_device *v) { }
static void v4l2_device_put(struct v4l2_device *v) { }
static void devm_free_irq(void *dev, int irq, void *data) { }
static void pm_runtime_disable(void *dev) { }
static bool pm_runtime_status_suspended(void *dev) { return true; }
static void rga1_runtime_suspend(void *dev) { }
static void pm_runtime_set_suspended(void *dev) { }
static void dma_free_coherent(void *dev, size_t size, void *cpu, uintptr_t dma) { }
static void v4l2_ctrl_handler_free(void *c) { }
static void v4l2_fh_del(void *fh, struct file *f) { }
static void v4l2_fh_exit(void *fh) { }
#define kfree free

__attribute__((noinline)) static void queue_link(struct v4l2_m2m_buffer *b)
{
    b->list.next = &b->list;
    b->list.prev = &b->list;
}
static int vb2_queue_init(struct vb2_queue *q)
{
    if (!q->buf_struct_size) q->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
    /* The allocation size is passed by the actual driver queue callback. */
    struct v4l2_m2m_buffer *b = calloc(1, q->buf_struct_size);
    assert(b);
    queue_link(b); /* ASan catches the old fallback's trailing-list write. */
    assert(b->list.next == &b->list && b->list.prev == &b->list);
    assert(q->buf_struct_size >= sizeof(*b));
    init_count++;
    free(b);
    return 0;
}
'''
for name in ['rga1_free_mmu', 'rga1_queues', 'rga1_release', 'rga1_remove']:
    out += function(name)
out += r'''
static void *worker(void *arg)
{
    pthread_mutex_lock(&gate);
    worker_at_tail = true;
    pthread_cond_broadcast(&event);
    while (!allow_tail) pthread_cond_wait(&event, &gate);
    /* Model job_finish's remaining context accesses after waking suspend. */
    assert(worker_ctx->marker == 0x619);
    worker_ctx->marker++;
    worker_done = true;
    pthread_cond_broadcast(&event);
    pthread_mutex_unlock(&gate);
    return NULL;
}
static void *remove_thread(void *arg)
{
    actor = REMOVE;
    rga1_remove(arg);
    return NULL;
}
static void *close_thread(void *arg)
{
    actor = CLOSE;
    assert(!rga1_release(arg));
    return NULL;
}
static void run(enum actor winner)
{
    pthread_t work, first_thread, second_thread;
    struct rga1_dev rga = { .mutex = { PTHREAD_MUTEX_INITIALIZER } };
    struct platform_device pdev = { .rga = &rga };
    struct rga1_ctx *ctx = calloc(1, sizeof(*ctx));
    struct file file = { .ctx = ctx };
    assert(ctx);
    ctx->rga = &rga;
    ctx->fh.m2m_ctx = calloc(1, sizeof(*ctx->fh.m2m_ctx));
    assert(ctx->fh.m2m_ctx);
    worker_ctx = ctx->fh.m2m_ctx;
    worker_ctx->marker = 0x619;
    first = winner;
    worker_at_tail = allow_tail = worker_done = first_flush = false;
    contender_attempted = contender_blocked = destroyed = paused = freed = false;
    freed_count = 0;
    assert(!pthread_create(&work, NULL, worker, NULL));
    pthread_mutex_lock(&gate);
    while (!worker_at_tail) pthread_cond_wait(&event, &gate);
    pthread_mutex_unlock(&gate);
    if (winner == REMOVE) assert(!pthread_create(&first_thread, NULL, remove_thread, &pdev));
    else assert(!pthread_create(&first_thread, NULL, close_thread, &file));
    pthread_mutex_lock(&gate);
    while (!first_flush) pthread_cond_wait(&event, &gate);
    pthread_mutex_unlock(&gate);
    if (winner == REMOVE) assert(!pthread_create(&second_thread, NULL, close_thread, &file));
    else assert(!pthread_create(&second_thread, NULL, remove_thread, &pdev));
    pthread_mutex_lock(&gate);
    while (!contender_attempted) pthread_cond_wait(&event, &gate);
    /* Close/removal must not cross the other callback's undrained worker. */
    assert(contender_blocked && !freed);
    allow_tail = true;
    pthread_cond_broadcast(&event);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(work, NULL));
    assert(!pthread_join(first_thread, NULL));
    assert(!pthread_join(second_thread, NULL));
    assert(rga.disconnected && destroyed && paused && freed_count == 1);
    pthread_mutex_destroy(&rga.mutex.raw);
}
int main(void)
{
    struct rga1_dev rga = {0};
    struct rga1_ctx ctx = { .rga = &rga };
    struct vb2_queue src = {0}, dst = {0};
    assert(!rga1_queues(&ctx, &src, &dst));
    assert(init_count == 2);
    run(REMOVE);
    run(CLOSE);
    puts("RGA1 lifetime: both m2m buffer allocations and remove/close scheduler-tail interleavings ASan/UBSan PASS");
    return 0;
}
'''
Path(sys.argv[2]).write_text(out)
