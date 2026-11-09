#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Compile the real driver format callbacks with host context/queue shims.
from pathlib import Path
import sys
src=Path(sys.argv[1]).read_text()
out='''#define RGA1_HOST_TEST
#include "rga1-hw.h"
#include <linux/videodev2.h>
#include "rga1-controls.h"
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#define strscpy(dst,src,n) snprintf((char *)(dst),n,"%s",src)
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define min(a,b) ((a)<(b)?(a):(b))
#define PAGE_SIZE 4096UL
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define clamp(n,lo,hi) ((n)<(lo)?(lo):(n)>(hi)?(hi):(n))
struct rga1_ctx { struct { void *m2m_ctx; } fh; struct v4l2_pix_format_mplane pix[2]; struct rga1_image intermediate; unsigned angle,hx,vy,operation,key,alpha_mode,rop,fading,dither,pd,key_min,key_max,pattern_size,csc_alias,zero_key,gradient_wrap,alpha_calc,mix_dest,rop_mode,pattern_origin,rotate_aa,guard_lead,guard_gap;
 size_t mask_size; void *mask_sgt; struct {size_t size;} *mask_buf;
 int gradient[8]; bool palette_expand; struct rga1_image expanded_image; bool hflip,vflip; struct v4l2_rect rect[2]; };
struct file { struct rga1_ctx *ctx; };
static int rga1_sg_check(void *p,size_t n) {(void)n;return p?0:-EINVAL;}
struct device;
struct vb2_queue { bool busy, streaming; unsigned type; struct rga1_ctx *ctx; } queues[2];
static struct rga1_ctx *vb2_get_drv_priv(struct vb2_queue *q) { return q->ctx; }
static struct rga1_ctx *rga1_ctx(struct file *f) { return f->ctx; }
static struct vb2_queue *v4l2_m2m_get_vq(void *p, unsigned t) { (void)p; return &queues[V4L2_TYPE_IS_OUTPUT(t)?0:1]; }
static struct vb2_queue *v4l2_m2m_get_src_vq(void *p) { return v4l2_m2m_get_vq(p,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE); }
static struct vb2_queue *v4l2_m2m_get_dst_vq(void *p) { return v4l2_m2m_get_vq(p,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE); }
static bool vb2_is_busy(struct vb2_queue *q) { return q->busy; }
static bool vb2_is_streaming(struct vb2_queue *q) { return q->streaming; }
'''
a=src.index('struct rga1_fmt {');b=src.index('static const struct rga1_fmt *rga1_format',a)
out+=src[a:b]
for name in ['rga1_format','rga1_capture_format','rga1_multiplanar','rga1_side','rga1_size','rga1_quant','rga1_try_fmt','rga1_s_fmt','rga1_enum_fmt','rga1_g_selection','rga1_s_selection','rga1_pre_shift','rga1_has_alpha','rga1_prepare_streaming','rga1_queue_setup']:
 pos=src.index(name+'(');start=src.rfind('static ',0,pos);brace=src.index('{',pos);depth=1;end=brace+1
 while depth:
  if src[end]=='{':depth+=1
  if src[end]=='}':depth-=1
  end+=1
 out+=src[start:end]+'\n'
out+=r'''
/* Use the same affine-pass image selected by the worker, independently of the
 * planner's guard calculation. Decode the actual emitted tile increments and
 * ensure the seed and complete tile footprint stay in the source/guard region,
 * before the destination mapping. Packed palette stride is never an RGB stride.
 */
static void affine_plan_bounds(const struct rga1_ctx *ctx) {
 struct rga1_image in=ctx->intermediate;
 if (!(ctx->hx || ctx->vy || ctx->palette_expand)) {
  in.width=ctx->rect[0].width;in.height=ctx->rect[0].height;
  in.stride=ctx->pix[0].plane_fmt[0].bytesperline;
 }
 in.y=ctx->guard_lead*PAGE_SIZE;
 struct rga1_image out={.width=ctx->rect[1].width,.height=ctx->rect[1].height,
  .format=RGA1_RGB24,.stride=ctx->rect[1].width*3};
 u32 cmd[32];
 rga1_blit_filter_halo(cmd,&in,&out,ctx->angle,ctx->hflip,ctx->vflip,0,1,1);
 s64 limit=(ctx->guard_lead+DIV_ROUND_UP(in.stride*in.height,PAGE_SIZE)+ctx->guard_gap)*PAGE_SIZE;
 for(unsigned corner=0;corner<4;corner++) {
  s64 tx=corner&1?(out.width-1)/8:0,ty=corner&2?(out.height-1)/8:0;
  s64 x=(int16_t)cmd[R1_TILE_H]*tx+(int16_t)cmd[R1_TILE_V]*ty;
  s64 y=(int16_t)(cmd[R1_TILE_H]>>16)*tx+(int16_t)(cmd[R1_TILE_V]>>16)*ty;
  s64 address=cmd[R1_SRC_Y]+(x>>10)*rga1_bpp(in.format)+(y>>10)*in.stride;
  /* A decoded tile has at most 23 samples/axis, plus the bus allowance. */
  assert(address>=64 && address+23*(in.stride+rga1_bpp(in.format))+64<limit);
 }
}
static int prepare(struct rga1_ctx *ctx) {
 for(unsigned side=0;side<2;side++) ctx->rect[side]=(struct v4l2_rect){.width=ctx->pix[side].width,.height=ctx->pix[side].height};
 return rga1_prepare_streaming(&queues[0]);
}
int main(void) {
 struct rga1_ctx ctx={0}; struct file file={.ctx=&ctx};
 struct v4l2_format out={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
 struct v4l2_format cap={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
 out.fmt.pix_mp.width=out.fmt.pix_mp.height=100;
 out.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_RGB24;
 assert(!rga1_s_fmt(&file,NULL,&out));
 /* Every advertised source format must round-trip in negotiation. */
 for(unsigned i=0;i<ARRAY_SIZE(rga1_formats);i++) {
  if(!rga1_formats[i].input)continue;
  out.fmt.pix_mp.pixelformat=rga1_formats[i].fourcc;
  assert(!rga1_s_fmt(&file,NULL,&out));
  assert(out.fmt.pix_mp.pixelformat==rga1_formats[i].fourcc);
  assert(out.fmt.pix_mp.plane_fmt[0].sizeimage >= out.fmt.pix_mp.width*out.fmt.pix_mp.height);
 }
 out.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_RGB24;
 assert(!rga1_s_fmt(&file,NULL,&out));
 struct v4l2_selection sel={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT,.target=V4L2_SEL_TGT_CROP,
   .r={.left=2,.top=2,.width=96,.height=96}};
 assert(!rga1_s_selection(&file,NULL,&sel));
 assert(!rga1_g_selection(&file,NULL,&sel));
 sel.r.left=-1; assert(rga1_s_selection(&file,NULL,&sel)==-EINVAL);
 sel.r.left=5; assert(rga1_s_selection(&file,NULL,&sel)==-EINVAL);
 sel.r.left=0; queues[0].streaming=true;
 assert(rga1_s_selection(&file,NULL,&sel)==-EBUSY); queues[0].streaming=false;
 /* The exact RGB passthrough invariant of upstream testM2MFormats. */
 out.fmt.pix_mp.colorspace=V4L2_COLORSPACE_SMPTE170M;
 out.fmt.pix_mp.ycbcr_enc=V4L2_YCBCR_ENC_709;
 out.fmt.pix_mp.xfer_func=V4L2_XFER_FUNC_709;
 out.fmt.pix_mp.quantization=V4L2_QUANTIZATION_LIM_RANGE;
 assert(!rga1_s_fmt(&file,NULL,&out));
 assert(out.fmt.pix_mp.quantization==V4L2_QUANTIZATION_LIM_RANGE);
 assert(ctx.pix[1].quantization==out.fmt.pix_mp.quantization);
 assert(ctx.pix[1].xfer_func==out.fmt.pix_mp.xfer_func);
 cap.fmt.pix_mp=out.fmt.pix_mp;
 cap.fmt.pix_mp.flags=V4L2_PIX_FMT_FLAG_SET_CSC;
 cap.fmt.pix_mp.quantization=V4L2_QUANTIZATION_FULL_RANGE;
 assert(!rga1_s_fmt(&file,NULL,&cap));
 assert(cap.fmt.pix_mp.quantization==V4L2_QUANTIZATION_LIM_RANGE);
 /* YUV limited -> RGB full requires explicit capture CSC negotiation. */
 out.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12;
 out.fmt.pix_mp.width=2560; out.fmt.pix_mp.height=1440;
 out.fmt.pix_mp.ycbcr_enc=V4L2_YCBCR_ENC_601;
 assert(!rga1_s_fmt(&file,NULL,&out));
 cap.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_RGB24;
 cap.fmt.pix_mp.width=1920; cap.fmt.pix_mp.height=1080;
 cap.fmt.pix_mp.flags=V4L2_PIX_FMT_FLAG_SET_CSC;
 cap.fmt.pix_mp.quantization=V4L2_QUANTIZATION_FULL_RANGE;
 assert(!rga1_s_fmt(&file,NULL,&cap));
 assert(rga1_quant(&ctx.pix[0])==V4L2_QUANTIZATION_LIM_RANGE);
 assert(rga1_quant(&ctx.pix[1])==V4L2_QUANTIZATION_FULL_RANGE);
 assert(ctx.pix[1].quantization==cap.fmt.pix_mp.quantization);
 struct v4l2_fmtdesc e={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
 assert(!rga1_enum_fmt(&file,NULL,&e));
 assert(e.flags&V4L2_FMT_FLAG_CSC_QUANTIZATION);
 /* DEFAULT resolves per format; unsupported 709 full is reported limited. */
 out.fmt.pix_mp.quantization=V4L2_QUANTIZATION_DEFAULT;
 assert(!rga1_s_fmt(&file,NULL,&out));
 assert(ctx.pix[0].quantization==V4L2_QUANTIZATION_DEFAULT);
 assert(rga1_quant(&ctx.pix[0])==V4L2_QUANTIZATION_LIM_RANGE);
 cap.fmt.pix_mp.flags=0;
 assert(!rga1_s_fmt(&file,NULL,&cap));
 assert(ctx.pix[1].quantization==V4L2_QUANTIZATION_DEFAULT);
 assert(rga1_quant(&ctx.pix[1])==V4L2_QUANTIZATION_FULL_RANGE);
 out.fmt.pix_mp.ycbcr_enc=V4L2_YCBCR_ENC_709;
 out.fmt.pix_mp.quantization=V4L2_QUANTIZATION_FULL_RANGE;
 assert(!rga1_s_fmt(&file,NULL,&out));
 assert(out.fmt.pix_mp.quantization==V4L2_QUANTIZATION_LIM_RANGE);
 /* Exercise the actual two-pass planner's hardware/format boundaries. */
 queues[0].ctx=&ctx;
 ctx.pix[0].width=3840; ctx.pix[0].height=2160;
 ctx.pix[1].width=1920; ctx.pix[1].height=1080;
 ctx.pix[1].quantization=V4L2_QUANTIZATION_FULL_RANGE;
 assert(!prepare(&ctx));
 assert(ctx.hx==1 && ctx.vy==1 && ctx.intermediate.width==1920);
 ctx.pix[0].width=2560; ctx.pix[0].height=1440; ctx.angle=90;
 assert(!prepare(&ctx));
 assert(ctx.hx==1 && ctx.vy==0 && ctx.intermediate.width==1280);
 ctx.pix[0].width=2562;
 assert(prepare(&ctx)==-EINVAL); /* odd NV12 scratch width */
 ctx.angle=0; ctx.pix[0].width=4098;
 assert(prepare(&ctx)==-EINVAL); /* fractional prescale */
 ctx.pix[0].width=8192; ctx.pix[0].height=8192;
 ctx.pix[1].width=1024; ctx.pix[1].height=1024;
 assert(!prepare(&ctx));
 assert(ctx.hx==3 && ctx.vy==3 && ctx.intermediate.width==1024);
 ctx.pix[1].width=512; ctx.pix[1].height=512;
 assert(prepare(&ctx)==-EINVAL); /* 2.14 endpoint overflow */
 ctx.pix[0].pixelformat=V4L2_PIX_FMT_RGB24;
 ctx.pix[0].quantization=V4L2_QUANTIZATION_FULL_RANGE;
 ctx.pix[0].width=2; ctx.pix[0].height=2;
 ctx.pix[1].width=9; ctx.pix[1].height=9;
 assert(!prepare(&ctx)); /* exact maximum 8x endpoint */
 ctx.pix[1].width=10;
 assert(prepare(&ctx)==-EINVAL);
 /* CREATE_BUFS may request larger planes; never shrink that allocation. */
 queues[0].type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
 ctx.pix[0].num_planes=3;
 for(unsigned i=0;i<3;i++)ctx.pix[0].plane_fmt[i].sizeimage=4096/(i+1);
 unsigned planes=3,count=1,sizes[3]={1048576,1048576,1048576};
 assert(!rga1_queue_setup(&queues[0],&count,&planes,sizes,NULL));
 for(unsigned i=0;i<3;i++)assert(sizes[i]==1048576);
 sizes[2]=1;assert(rga1_queue_setup(&queues[0],&count,&planes,sizes,NULL)==-EINVAL);
 planes=1;assert(rga1_queue_setup(&queues[0],&count,&planes,sizes,NULL)==-EINVAL);
 planes=0;assert(!rga1_queue_setup(&queues[0],&count,&planes,sizes,NULL));
 assert(planes==3);
 for(unsigned i=0;i<3;i++)assert(sizes[i]==ctx.pix[0].plane_fmt[i].sizeimage);
 /* ROP4 accepts only a complete, sufficiently large imported mask and
  * unchanged full-image geometry, before streaming allocates anything. */
 ctx=(struct rga1_ctx){0};queues[0].ctx=&ctx;
 for(unsigned side=0;side<2;side++)ctx.pix[side]=(struct v4l2_pix_format_mplane){
  .width=64,.height=8,.pixelformat=V4L2_PIX_FMT_RGB24,.num_planes=1,
  .ycbcr_enc=V4L2_YCBCR_ENC_601,.quantization=V4L2_QUANTIZATION_FULL_RANGE};
 ctx.rop=1;ctx.rop_mode=2;ctx.pattern_size=(1U<<16)|1;
 assert(prepare(&ctx)==-EINVAL);
 typeof(*ctx.mask_buf) mask={.size=63};ctx.mask_buf=&mask;ctx.mask_sgt=(void*)1;
 assert(prepare(&ctx)==-EINVAL);
 mask.size=64;assert(!prepare(&ctx));assert(ctx.mask_size==64);
 ctx.rect[0].left=1;ctx.rect[0].width=63;
 assert(rga1_prepare_streaming(&queues[0])==-EINVAL);
 ctx.angle=1;assert(prepare(&ctx)==-EOPNOTSUPP);
 /* #725 review: palette expansion supplies RGB scratch to arbitrary rotation.
  * Cover the reported PAL1 2048x4 at89deg and transposed/prescaled variants,
  * every integer non-quarter angle and each reflection, using real planner and
  * emitter code. No hardware or test-only driver path is substituted. */
 const uint32_t palettes[]={V4L2_PIX_FMT_RGA1_PAL1,V4L2_PIX_FMT_RGA1_PAL2,V4L2_PIX_FMT_RGA1_PAL4};
 const unsigned shapes[][4]={{2048,4,2048,4},{4,2048,4,2048},
  {2048,16,1024,8},{4,2048,4,1024}};
 unsigned affine_cases=0;
 for(unsigned p=0;p<3;p++)for(unsigned shape=0;shape<4;shape++) {
  ctx=(struct rga1_ctx){0};queues[0].ctx=&ctx;
  for(unsigned side=0;side<2;side++)ctx.pix[side]=(struct v4l2_pix_format_mplane){
   .width=shapes[shape][side*2],.height=shapes[shape][side*2+1],
   .pixelformat=side?V4L2_PIX_FMT_RGB24:palettes[p],.num_planes=1,
   .ycbcr_enc=V4L2_YCBCR_ENC_601,.quantization=V4L2_QUANTIZATION_FULL_RANGE};
  ctx.pix[0].plane_fmt[0].bytesperline=ALIGN(DIV_ROUND_UP(ctx.pix[0].width*(1U<<p),8),4);
  for(unsigned angle=1;angle<360;angle++)if(angle%90)
   for(unsigned flip=0;flip<4;flip++) {
    ctx.angle=angle;ctx.hflip=flip&1;ctx.vflip=flip&2;
    assert(!prepare(&ctx));assert(ctx.palette_expand);
    assert(ctx.intermediate.format==RGA1_RGB24);
    assert(ctx.intermediate.stride==ctx.intermediate.width*3);
    affine_plan_bounds(&ctx);affine_cases++;
   }
 }
 /* #725 review: RGB source alone does not make a YUV destination safe for
  * the packed-RGB canvas fill. Reject both 420/422 and UV/VU capture before
  * allocation, including exactly RGB24 2048x2 -> NV16 2048x2 at45deg. */
 const uint32_t yuv[]={V4L2_PIX_FMT_NV12,V4L2_PIX_FMT_NV21,V4L2_PIX_FMT_NV16,V4L2_PIX_FMT_NV61};
 unsigned rejected=0;
 for(unsigned f=0;f<4;f++) {
  ctx=(struct rga1_ctx){0};queues[0].ctx=&ctx;
  for(unsigned side=0;side<2;side++)ctx.pix[side]=(struct v4l2_pix_format_mplane){
   .width=2048,.height=2,.pixelformat=side?yuv[f]:V4L2_PIX_FMT_RGB24,.num_planes=1,
   .ycbcr_enc=V4L2_YCBCR_ENC_601,.quantization=V4L2_QUANTIZATION_FULL_RANGE};
  ctx.pix[0].plane_fmt[0].bytesperline=2048*3;
  assert(!prepare(&ctx));
  for(unsigned angle=1;angle<360;angle++)if(angle%90) {
   ctx.angle=angle;assert(prepare(&ctx)==-EOPNOTSUPP);rejected++;
  }
  ctx.angle=90;
  ctx.pix[0].width=ctx.pix[0].height=ctx.pix[1].width=ctx.pix[1].height=64;
  assert(!prepare(&ctx));
 }
 printf("RGA1 affine planner: %u expanded-palette bounds and %u pre-DMA YUV refusals PASS\n",affine_cases,rejected);
 puts("RGA1 format negotiation, three-plane CREATE_BUFS sizing and exact selection bounds PASS");
 puts("RGA1 scaling bounds: two-pass, rotated, fractional/chroma and endpoint limits PASS");
 puts("RGA1 V4L2 colorimetry: RGB passthrough, YUV capture CSC, defaults and unsupported-range negotiation PASS");
 return 0;
}
'''
Path(sys.argv[2]).write_text(out)
