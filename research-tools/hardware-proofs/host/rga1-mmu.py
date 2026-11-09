#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual internal-MMU mapper with non-contiguous DMA segments."""
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
#include <stdint.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(n) (((n)+PAGE_SIZE-1)&~(PAGE_SIZE-1))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define IS_ALIGNED(n,a) (!((n)&((a)-1)))
#define upper_32_bits(n) ((uint64_t)(n)>>32)
struct scatterlist { dma_addr_t dma; unsigned len; };
struct sg_table { struct scatterlist *sgl; unsigned nents; };
#define sg_dma_address(s) ((s)->dma)
#define sg_dma_len(s) ((s)->len)
#define for_each_sg(list,sg,n,i) for((i)=0,(sg)=(list);(i)<(n);(i)++,(sg)++)
struct vb2_buffer { struct sg_table *sgt[3]; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
static struct sg_table *vb2_dma_sg_plane_desc(struct vb2_buffer *b, int p) { (void)p; return b->sgt[p]; }
struct rga1_ctx {
 struct v4l2_pix_format_mplane pix[2];
 size_t scratch_size, page_table_size, mask_size,expanded_size;
 bool from_palette,palette_first;unsigned mask_base,guard_lead,guard_gap; struct sg_table *mask_sgt;
 u32 *page_table;
 dma_addr_t scratch_dma, guard_dma,expanded_dma;
};
'''
for name in ['rga1_pages', 'rga1_sg_check', 'rga1_map_sg', 'rga1_map_buffer', 'rga1_map_pass']:
    out += function(name)
out += r'''
int main(void)
{
 struct scatterlist a[]={{0x12345000,4096},{0x22345000,8192},{0x32345000,4096}};
 struct scatterlist b[]={{0x98765000,4096},{0x88765000,4096}};
 struct sg_table sa={a,3}, sb={b,2};
 assert(!rga1_sg_check(&sa,16383));
 assert(rga1_sg_check(&sa,16385)==-EINVAL);
 a[0].dma++; assert(rga1_sg_check(&sa,1)==-EINVAL); a[0].dma--;
 a[0].len--; assert(rga1_sg_check(&sa,1)==-EINVAL); a[0].len++;
 a[0].dma=0xfffff000ULL; a[0].len=8192;
 assert(rga1_sg_check(&sa,1)==-EINVAL);
 a[0].dma=0x12345000; a[0].len=4096;
 struct rga1_ctx ctx={.scratch_size=12287,.scratch_dma=0x61000000,
    .page_table_size=4096,.guard_dma=0x71000000};
 ctx.guard_lead=ctx.guard_gap=16;ctx.pix[0].num_planes=ctx.pix[1].num_planes=1;
 ctx.pix[0].plane_fmt[0].sizeimage=16383;
 ctx.pix[1].plane_fmt[0].sizeimage=8191;
 ctx.page_table=malloc(ctx.page_table_size); assert(ctx.page_table);
 struct vb2_v4l2_buffer src={.vb2_buf={{&sa}}},dst={.vb2_buf={{&sb}}};
 u32 base=rga1_map_pass(&ctx,&src,&dst,false);
 assert(base==36*PAGE_SIZE);
 const u32 expected[]={0x12345000,0x22345000,0x22346000,0x32345000};
 for(unsigned i=0;i<16;i++)assert(ctx.page_table[i]==ctx.guard_dma);
 for(unsigned i=0;i<4;i++) assert(ctx.page_table[16+i]==expected[i]);
 for(unsigned i=20;i<36;i++) assert(ctx.page_table[i]==ctx.guard_dma);
 assert(ctx.page_table[36]==0x98765000 && ctx.page_table[37]==0x88765000);
 for(unsigned i=38;i<1024;i++) assert(ctx.page_table[i]==ctx.guard_dma);
 base=rga1_map_pass(&ctx,&src,&dst,true);
 assert(base==36*PAGE_SIZE);
 for(unsigned i=0;i<3;i++) assert(ctx.page_table[36+i]==ctx.scratch_dma+i*PAGE_SIZE);
 base=rga1_map_pass(&ctx,NULL,&dst,false);
 assert(base==35*PAGE_SIZE);
 for(unsigned i=0;i<16;i++)assert(ctx.page_table[i]==ctx.guard_dma);
 for(unsigned i=0;i<3;i++) assert(ctx.page_table[16+i]==ctx.scratch_dma+i*PAGE_SIZE);
 assert(ctx.page_table[35]==0x98765000 && ctx.page_table[36]==0x88765000);
 for(unsigned i=37;i<1024;i++) assert(ctx.page_table[i]==ctx.guard_dma);
 /* Three independent memory planes, a noncontiguous mask and enlarged
  * affine guards must never alias each other or the destination. */
 struct scatterlist uv_a[]={{0x42345000,4096}},v_a[]={{0x52345000,4096}};
 struct scatterlist uv_b[]={{0xa8765000,4096}},v_b[]={{0xb8765000,4096}};
 struct sg_table sua={uv_a,1},sva={v_a,1},sub={uv_b,1},svb={v_b,1};
 src.vb2_buf.sgt[1]=&sua;src.vb2_buf.sgt[2]=&sva;
 dst.vb2_buf.sgt[1]=&sub;dst.vb2_buf.sgt[2]=&svb;
 ctx.pix[0].num_planes=ctx.pix[1].num_planes=3;
 ctx.pix[0].plane_fmt[0].sizeimage=5000;
 ctx.pix[0].plane_fmt[1].sizeimage=ctx.pix[0].plane_fmt[2].sizeimage=1000;
 ctx.pix[1].plane_fmt[0].sizeimage=6000;
 ctx.pix[1].plane_fmt[1].sizeimage=ctx.pix[1].plane_fmt[2].sizeimage=1000;
 ctx.guard_lead=128;ctx.guard_gap=256;ctx.mask_sgt=&sb;ctx.mask_size=8191;
 base=rga1_map_pass(&ctx,&src,&dst,false);
 assert(ctx.mask_base==388*PAGE_SIZE && base==406*PAGE_SIZE);
 assert(ctx.page_table[128]==0x12345000 && ctx.page_table[129]==0x22345000);
 assert(ctx.page_table[130]==0x42345000 && ctx.page_table[131]==0x52345000);
 assert(ctx.page_table[388]==0x98765000 && ctx.page_table[389]==0x88765000);
 assert(ctx.page_table[406]==0x98765000 && ctx.page_table[407]==0x88765000);
 assert(ctx.page_table[408]==0xa8765000 && ctx.page_table[409]==0xb8765000);
 for(unsigned i=132;i<388;i++)assert(ctx.page_table[i]==ctx.guard_dma);
 for(unsigned i=390;i<406;i++)assert(ctx.page_table[i]==ctx.guard_dma);
 for(unsigned i=410;i<1024;i++)assert(ctx.page_table[i]==ctx.guard_dma);
 /* Palette expansion -> average prescale -> ordinary capture uses two
  * distinct coherent images and exactly the intended source at each stage. */
 ctx.mask_size=0;ctx.expanded_size=16384;ctx.expanded_dma=0xc0000000;
 ctx.palette_first=true;ctx.from_palette=false;
 base=rga1_map_pass(&ctx,&src,&dst,true);
 assert(base==388*PAGE_SIZE && ctx.page_table[388]==0xc0000000);
 ctx.palette_first=false;ctx.from_palette=true;
 base=rga1_map_pass(&ctx,NULL,&dst,true);
 assert(base==388*PAGE_SIZE && ctx.page_table[128]==0xc0000000);
 assert(ctx.page_table[388]==ctx.scratch_dma);
 ctx.from_palette=false;
 base=rga1_map_pass(&ctx,NULL,&dst,false);
 assert(base==387*PAGE_SIZE && ctx.page_table[128]==ctx.scratch_dma);
 assert(ctx.page_table[387]==0x98765000);
 free(ctx.page_table);
 puts("RGA1 MMU: discontiguous DMA pages, invalid exporters, three-plane images, mask/palette stages and private affine guard gaps ASan/UBSan PASS");
 return 0;
}
'''
Path(sys.argv[2]).write_text(out)
