#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the real mask importer against failing DMA-BUF ownership boundaries."""
from pathlib import Path
import re
import sys
src=Path(sys.argv[1]).read_text()
def function(name):
    pos=re.search(r'\b'+name+r'\s*\([^;{]*\)\s*\{',src).start()
    a=src.rfind('static ',0,pos); b=src.index('{',pos)+1; depth=1
    while depth:
        depth+=(src[b]=='{')-(src[b]=='}');b+=1
    return src[a:b]+'\n'
out=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "rga1-controls.h"
typedef uint64_t dma_addr_t;
typedef uint64_t u64;
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(n) (((n)+4095)&~4095UL)
#define IS_ALIGNED(n,a) (!((n)&((a)-1)))
#define upper_32_bits(n) ((uint64_t)(n)>>32)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define DMA_TO_DEVICE 1
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(n) ((void*)(intptr_t)(n))
struct scatterlist {dma_addr_t dma;size_t len;};
struct sg_table {struct scatterlist *sgl;unsigned nents;};
#define sg_dma_address(s) ((s)->dma)
#define sg_dma_len(s) ((s)->len)
#define for_each_sg(list,sg,n,i) for((i)=0,(sg)=(list);(i)<(n);(i)++,(sg)++)
struct dma_buf {size_t size;unsigned refs,attached,mapped,fd;};
struct dma_buf_attachment {struct dma_buf *buf;bool mapped;};
struct rga1_ctx {
 struct {void *m2m_ctx;} fh;
 struct {void *dev;} *rga;
 struct dma_buf *mask_buf;
 struct dma_buf_attachment *mask_attach;
 struct sg_table *mask_sgt;
};
struct file {struct rga1_ctx *ctx;};
struct vb2_queue {bool streaming;} queues[2];
static struct rga1_ctx *rga1_ctx(struct file *file) {return file->ctx;}
static bool vb2_is_streaming(struct vb2_queue *q) {return q->streaming;}
static struct vb2_queue *v4l2_m2m_get_src_vq(void *p) {(void)p;return &queues[0];}
static struct vb2_queue *v4l2_m2m_get_dst_vq(void *p) {(void)p;return &queues[1];}
static struct dma_buf buffers[6];
static struct scatterlist segments[2]={{0x12345000,4096},{0x22345000,4096}};
static struct sg_table good={segments,2};
static struct scatterlist invalid_segments[1]={{0x32345001,4096}};
static struct sg_table bad={invalid_segments,1};
static struct dma_buf *dma_buf_get(int fd)
{
 if(fd<0||fd>=6||fd==4)return ERR_PTR(-EBADF);
 buffers[fd].refs++;return &buffers[fd];
}
static void dma_buf_put(struct dma_buf *b) {assert(b->refs);b->refs--;}
static struct dma_buf_attachment *dma_buf_attach(struct dma_buf *b,void *dev)
{
 (void)dev;if(b->fd==2)return ERR_PTR(-EIO);
 struct dma_buf_attachment *a=malloc(sizeof(*a));assert(a);a->buf=b;a->mapped=false;b->attached++;return a;
}
static void dma_buf_detach(struct dma_buf *b,struct dma_buf_attachment *a)
{assert(b==a->buf&&b->attached&&!a->mapped);b->attached--;free(a);}
static struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *a,int dir)
{
 assert(dir==DMA_TO_DEVICE);if(a->buf->fd==3)return ERR_PTR(-ENOMEM);
 a->mapped=true;a->buf->mapped++;return a->buf->fd==5?&bad:&good;
}
static void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *a,struct sg_table *s,int dir)
{assert(dir==DMA_TO_DEVICE&&a->mapped&&a->buf->mapped&&(s==&good||s==&bad));a->mapped=false;a->buf->mapped--;}
'''
for name in ['rga1_sg_check','rga1_put_mask','rga1_set_mask','rga1_default']:
    out+=function(name)
out+=r'''
static void balanced(unsigned owned)
{
 for(unsigned i=0;i<6;i++) {
  unsigned n=i==owned;
  assert(buffers[i].refs==n&&buffers[i].attached==n&&buffers[i].mapped==n);
 }
}
int main(void)
{
 struct rga1_ctx ctx={0};struct {void *dev;} dev={0};ctx.rga=(void*)&dev;
 for(unsigned i=0;i<6;i++){buffers[i].fd=i;buffers[i].size=8192;}
 assert(!rga1_set_mask(&ctx,0));balanced(0);
 struct dma_buf *old=ctx.mask_buf;
 for(int fd=2;fd<=5;fd++) {
  assert(rga1_set_mask(&ctx,fd)<0);assert(ctx.mask_buf==old);balanced(0);
 }
 assert(rga1_set_mask(&ctx,-2)==-EBADF);balanced(0);
 /* Same numeric fd reacquires its object rather than skipping validation. */
 assert(!rga1_set_mask(&ctx,0));balanced(0);
 assert(!rga1_set_mask(&ctx,1));assert(ctx.mask_buf==&buffers[1]);balanced(1);
 assert(!rga1_set_mask(&ctx,-1));balanced(99);
 assert(!rga1_set_mask(&ctx,-1));balanced(99);
 assert(!rga1_set_mask(&ctx,0));rga1_put_mask(&ctx);balanced(99);
 struct file file={.ctx=&ctx};struct rga1_mask mask={.fd=0};
 assert(sizeof(mask)==8);
 assert(rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK+1,&mask)==-ENOTTY);
 mask.flags=1;assert(rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask)==-EINVAL);
 mask.flags=0;mask.fd=-2;assert(rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask)==-EINVAL);
 mask.fd=0;
 for(unsigned side=0;side<2;side++) {
  queues[side].streaming=true;
  assert(rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask)==-EBUSY);balanced(99);
  queues[side].streaming=false;
 }
 assert(!rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask));balanced(0);
 mask.fd=4;assert(rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask)==-EBADF);balanced(0);
 mask.fd=-1;assert(!rga1_default(&file,NULL,true,VIDIOC_RGA1_SET_MASK,&mask));balanced(99);
 puts("RGA1 mask DMA-BUF: get/attach/map/SG failures preserve ownership, replacement/release/close ASan/UBSan PASS");
 return 0;
}
'''
Path(sys.argv[2]).write_text(out)
