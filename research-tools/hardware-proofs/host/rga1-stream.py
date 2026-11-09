#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fault-inject all actual stream allocations and retry with new geometry."""
from pathlib import Path
import re
import sys
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
#define RGA1_HOST_TEST
#include "rga1-hw.h"
#include <linux/videodev2.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(n) (((n)+4095)&~4095UL)
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define GFP_KERNEL 0
#define WRITE_ONCE(dst,v) ((dst)=(v))
#define VB2_BUF_STATE_QUEUED 1
#define VB2_BUF_STATE_ERROR 2
#define dev_info_once(...) ((void)0)
typedef uintptr_t dma_addr_t;
struct rga1_dev {void *dev,*regs; bool disconnected,broken;};
struct rga1_ctx {
 struct rga1_dev *rga;
 struct {void *m2m_ctx;} fh;
 struct v4l2_pix_format_mplane pix[2];
 struct rga1_image intermediate,expanded_image;
 unsigned hx,vy,sequence[2],guard_lead,guard_gap,operation;bool aborting,palette_expand;
 void *scratch,*guard,*aux,*expanded;
 u32 *page_table;
 dma_addr_t scratch_dma,guard_dma,aux_dma,page_table_dma,expanded_dma;
 size_t scratch_size,page_table_size,mask_size,expanded_size;
};
struct vb2_queue {struct rga1_ctx *ctx;unsigned type;bool streaming;};
static struct vb2_queue queues[2];
static struct rga1_ctx *vb2_get_drv_priv(struct vb2_queue *q) {return q->ctx;}
static bool vb2_is_streaming(struct vb2_queue *q) {return q->streaming;}
static struct vb2_queue *v4l2_m2m_get_src_vq(void *ctx) {(void)ctx;return &queues[0];}
static struct vb2_queue *v4l2_m2m_get_dst_vq(void *ctx) {(void)ctx;return &queues[1];}
static unsigned rga1_side(unsigned type) {return V4L2_TYPE_IS_OUTPUT(type)?0:1;}
static unsigned allocations,fail_at,live,pm_refs,return_count;
static int pm_error;
static struct {void *cpu;size_t size;} records[64];
static int pm_runtime_resume_and_get(void *dev) {(void)dev;if(pm_error)return pm_error;pm_refs++;return 0;}
static void pm_runtime_put(void *dev) {(void)dev;assert(pm_refs);pm_refs--;}
static void rga1_return_buffers(struct vb2_queue *q,int state) {(void)q;(void)state;return_count++;}
static void *dma_alloc_coherent(void *dev,size_t size,dma_addr_t *dma,int flags)
{
 (void)dev;(void)flags;allocations++;if(allocations==fail_at)return NULL;
 assert(allocations<64);void *p=calloc(1,PAGE_ALIGN(size));assert(p);
 records[allocations].cpu=p;records[allocations].size=size;live++;
 *dma=0x60000000+allocations*0x100000;return p;
}
static void dma_free_coherent(void *dev,size_t size,void *p,dma_addr_t dma)
{
 (void)dev;(void)dma;assert(p);bool found=false;
 for(unsigned i=0;i<64;i++)if(records[i].cpu==p){assert(records[i].size==size);records[i].cpu=NULL;found=true;break;}
 assert(found&&live);live--;free(p);
}
'''
for name in ['rga1_pages', 'rga1_free_mmu', 'rga1_start_streaming', 'rga1_stop_streaming']:
    out += function(name)
out += r'''
int main(void)
{
 struct rga1_dev dev={0};
 for(unsigned palette=0;palette<2;palette++)for(unsigned fault=1;fault<=4+palette;fault++) {
  struct rga1_ctx ctx={.rga=&dev,.hx=1,.vy=1,
   .intermediate={.format=RGA1_RGB24,.width=16,.height=16,.stride=48}};
  ctx.palette_expand=palette;ctx.expanded_image=(struct rga1_image){.width=32,.height=32,.stride=128,.format=RGA1_RGBA32};
  ctx.guard_lead=ctx.guard_gap=16;ctx.pix[0].num_planes=ctx.pix[1].num_planes=1;
  ctx.pix[0].plane_fmt[0].sizeimage=16384;ctx.pix[1].plane_fmt[0].sizeimage=4096;
  queues[0]=(struct vb2_queue){&ctx,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,false};
  queues[1]=(struct vb2_queue){&ctx,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,false};
  allocations=0;fail_at=fault;pm_refs=live=return_count=0;pm_error=0;
  assert(rga1_start_streaming(&queues[0],1)==-ENOMEM);
  assert(!ctx.scratch&&!ctx.scratch_size&&!ctx.expanded&&!ctx.expanded_size&&!ctx.page_table&&!ctx.guard&&!ctx.aux);
  assert(!live&&!pm_refs&&return_count==1);
  /* After first-queue failure, V4L2 permits changing controls/formats. */
  ctx.hx=ctx.vy=0;ctx.palette_expand=false;ctx.mask_size=524288;
  ctx.pix[0].plane_fmt[0].sizeimage=268435456;ctx.pix[1].plane_fmt[0].sizeimage=16777216;
  fail_at=0;
  assert(!rga1_start_streaming(&queues[0],1));
  assert(ctx.page_table_size==PAGE_ALIGN((65536+4096+128+64)*sizeof(u32)));
  assert(!ctx.scratch&&ctx.page_table&&ctx.guard&&ctx.aux&&live==3&&pm_refs==1);
  queues[0].streaming=true;
  /* A failed second queue must preserve the first queue's live allocation. */
  pm_error=-EIO;
  assert(rga1_start_streaming(&queues[1],1)==-EIO);
  assert(ctx.page_table&&ctx.guard&&ctx.aux&&live==3&&pm_refs==1);
  pm_error=0;
  queues[0].streaming=false;rga1_stop_streaming(&queues[0]);
  assert(!ctx.page_table&&!ctx.guard&&!ctx.aux&&!live&&!pm_refs);
 }
 puts("RGA1 STREAMON: all ordinary/palette allocation failures, changed-geometry retry, second-queue failure and exact cleanup ASan/UBSan PASS");
 return 0;
}
'''
Path(sys.argv[2]).write_text(out)
