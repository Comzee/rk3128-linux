// SPDX-License-Identifier: MIT
/* Finite, generated-fixture RGA1 format/operation proofs. Shares transport,
 * independently checks exact bytes and documented arithmetic contracts. */
#define main rga1_baseline_main
#include "rga1-test.c"
#undef main
#include <linux/dma-heap.h>
struct setting { uint32_t id; int value; void *data; unsigned size; bool close_after; };
static uint8_t *job(const char *node, const struct test *t,
                    const uint8_t *raw, const uint8_t *initial,
                    const struct setting *settings, unsigned count,
                    const struct v4l2_rect *crop, const struct v4l2_rect *compose,
                    bool system_heap)
{
 enum v4l2_buf_type output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
 int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
 if (fd < 0) die(node);
 /* Planar CAPTURE is negotiated only for the explicit prescale operation. */
 for (unsigned i = 0; i < count; i++) if (settings[i].id == V4L2_CID_RGA1_OPERATION) {
  struct v4l2_control c = {.id=settings[i].id,.value=settings[i].value};
  must(fd, VIDIOC_S_CTRL, &c, "operation before format");
 }
 configure(fd, t);
 for (unsigned i = 0; i < count; i++) {
  struct v4l2_control c = { .id = settings[i].id, .value = settings[i].value };
  if (settings[i].id == VIDIOC_RGA1_SET_MASK) {
   struct rga1_mask mask={.fd=settings[i].value};
   must(fd,VIDIOC_RGA1_SET_MASK,&mask,"mask DMA-BUF import");
  } else if (settings[i].data) {
   struct v4l2_ext_control ec = { .id = settings[i].id, .size = settings[i].size, .ptr = settings[i].data };
   struct v4l2_ext_controls ecs = { .count = 1, .controls = &ec };
   must(fd, VIDIOC_S_EXT_CTRLS, &ecs, "operation data control");
  } else must(fd, VIDIOC_S_CTRL, &c, "operation control");
  if (settings[i].close_after && close(settings[i].value)) die("close imported fd");
 }
 if (crop || compose) for (unsigned side = 0; side < 2; side++) {
  const struct v4l2_rect *r = side ? compose : crop;
  if (!r) continue;
  struct v4l2_selection sel = { .type = side ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT,
    .target = side ? V4L2_SEL_TGT_COMPOSE : V4L2_SEL_TGT_CROP, .r = *r };
  must(fd, VIDIOC_S_SELECTION, &sel, "selection");
  if (memcmp(&sel.r, r, sizeof(*r))) { fprintf(stderr,"selection adjusted\n"); exit(1); }
 }
 struct memory src, dst;
 if (system_heap) {
  int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
  if (heap < 0) die("system DMA heap");
  for (unsigned side = 0; side < 2; side++) {
   struct dma_heap_allocation_data a = { .len = (sizeimage(side ? t->output : t->input,
    side ? t->dw : t->sw, side ? t->dh : t->sh) + 4095) & ~4095U, .fd_flags = O_RDWR | O_CLOEXEC };
   must(heap, DMA_HEAP_IOCTL_ALLOC, &a, "system heap allocation");
   struct memory *m = side ? &dst : &src;
   m->fd = a.fd; m->len = a.len;
   m->addr = mmap(NULL, m->len, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
   if (m->addr == MAP_FAILED) die("system heap mmap");
  }
  close(heap);
 } else { src = alloc_mmap(fd, output); dst = alloc_mmap(fd, capture); }
 size_t n = sizeimage(t->output, t->dw, t->dh);
 uint8_t *result = malloc(n);
 if (!result) die("malloc result");
 sync_mem(&src, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
 memset(src.addr, 0xa5, src.len);
 memcpy(src.addr, raw, sizeimage(t->input, t->sw, t->sh));
 sync_mem(&src, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
 sync_mem(&dst, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
 memset(dst.addr, 0xa5, dst.len);
 if (initial) memcpy(dst.addr, initial, n);
 sync_mem(&dst, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
 struct test transport = *t; transport.dmabuf = system_heap;
 queue(fd, output, &src, &transport); queue(fd, capture, &dst, &transport);
 must(fd, VIDIOC_STREAMON, &output, "STREAMON output");
 must(fd, VIDIOC_STREAMON, &capture, "STREAMON capture");
 for(unsigned i=0;i<count;i++)if(settings[i].id==VIDIOC_RGA1_SET_MASK) {
  struct rga1_mask release={.fd=-1};
  if(xioctl(fd,VIDIOC_RGA1_SET_MASK,&release)!=-1||errno!=EBUSY) {
   fprintf(stderr,"streaming mask replacement was accepted\n");exit(1);
  }
 }
 dequeue(fd, output, system_heap); dequeue(fd, capture, system_heap);
 must(fd, VIDIOC_STREAMOFF, &output, "STREAMOFF output");
 must(fd, VIDIOC_STREAMOFF, &capture, "STREAMOFF capture");
 sync_mem(&src, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
 size_t input_n = sizeimage(t->input, t->sw, t->sh);
 if (memcmp(src.addr, raw, input_n)) { fprintf(stderr,"DMA changed source image\n"); exit(1); }
 for (size_t i=input_n;i<src.len;i++) if (((uint8_t*)src.addr)[i]!=0xa5) {
  fprintf(stderr,"DMA changed source allocation tail\n");exit(1);
 }
 sync_mem(&src, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
 sync_mem(&dst, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
 for (size_t i=n;i<dst.len;i++) if (((uint8_t*)dst.addr)[i]!=0xa5) {
  fprintf(stderr,"DMA changed destination allocation tail\n");exit(1);
 }
 memcpy(result, dst.addr, n);
 sync_mem(&dst, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
 munmap(src.addr, src.len); munmap(dst.addr, dst.len); close(src.fd); close(dst.fd); close(fd);
 return result;
}
static void exact(const char *label, const uint8_t *a, const uint8_t *e, size_t size, unsigned tolerance)
{
 unsigned errors = 0, max = 0;
 for (size_t i = 0; i < size; i++) {
  unsigned d = abs((int)a[i] - e[i]);
  if (d > max) max = d;
  if (d > tolerance) {
   if (errors < 4) fprintf(stderr,"%s byte %zu: actual=%u expected=%u\n",label,i,a[i],e[i]);
   errors++;
  }
 }
 printf("{\"proof\":\"%s\",\"bytes\":%zu,\"max_error\":%u,\"errors\":%u,\"pass\":%s}\n",label,size,max,errors,errors?"false":"true"); fflush(stdout);
 if (errors) {
  static unsigned dumps;
  const char *prefix=getenv("RGA1_DUMP_FAIL");
  if (prefix && dumps<8) {
   char name[256];
   snprintf(name,sizeof(name),"%s-%u.actual.bin",prefix,dumps);
   FILE *f=fopen(name,"wb");if(!f||fwrite(a,1,size,f)!=size||fclose(f))die("failure capture");
   snprintf(name,sizeof(name),"%s-%u.expected.bin",prefix,dumps++);
   f=fopen(name,"wb");if(!f||fwrite(e,1,size,f)!=size||fclose(f))die("reference capture");
  }
  failures++;
 }
}
static void probe565(const char *node)
{
 struct test t = { .sw=256,.sh=256,.dw=256,.dh=256,.input=V4L2_PIX_FMT_RGB565,.output=V4L2_PIX_FMT_RGB24,.matrix=601 };
 uint16_t raw[65536];
 uint8_t zero[65536*3],replicate[65536*3];
 for (unsigned v=0;v<65536;v++) {
  raw[v]=v;
  unsigned r=v>>11,g=(v>>5)&63,b=v&31;
  zero[3*v]=r<<3; zero[3*v+1]=g<<2; zero[3*v+2]=b<<3;
  replicate[3*v]=(r<<3)|(r>>2); replicate[3*v+1]=(g<<2)|(g>>4); replicate[3*v+2]=(b<<3)|(b>>2);
 }
 uint8_t *actual=job(node,&t,(uint8_t*)raw,NULL,NULL,0,NULL,NULL,false);
 exact("RGB565 exhaustive zero-fill model",actual,zero,sizeof(zero),0);
 unsigned mismatch=0;
 for (unsigned i=0;i<sizeof(zero);i++) if(actual[i]!=replicate[i]) mismatch++;
 printf("{\"proof\":\"RGB565 competing bit-replication model\",\"mismatching_channels\":%u,\"channels\":%zu}\n",mismatch,sizeof(zero));
 free(actual);
}
static void format_suite(const char *node)
{
 const uint32_t formats[]={ V4L2_PIX_FMT_RGB565,V4L2_PIX_FMT_RGBA32,V4L2_PIX_FMT_RGBX32,V4L2_PIX_FMT_ABGR32,V4L2_PIX_FMT_XBGR32,V4L2_PIX_FMT_ARGB32,V4L2_PIX_FMT_XRGB32,V4L2_PIX_FMT_BGRA32,V4L2_PIX_FMT_BGRX32,
 V4L2_PIX_FMT_RGBA555,V4L2_PIX_FMT_RGBX555,V4L2_PIX_FMT_BGRA555,V4L2_PIX_FMT_BGRX555,
 V4L2_PIX_FMT_ARGB444,V4L2_PIX_FMT_XRGB444,V4L2_PIX_FMT_ABGR444,V4L2_PIX_FMT_XBGR444,V4L2_PIX_FMT_RGBA444,V4L2_PIX_FMT_RGBX444,V4L2_PIX_FMT_BGRA444,V4L2_PIX_FMT_BGRX444,
 V4L2_PIX_FMT_NV21,V4L2_PIX_FMT_NV61,V4L2_PIX_FMT_YUV420,V4L2_PIX_FMT_YVU420,V4L2_PIX_FMT_YUV422P };
 struct test t={.sw=128,.sh=96,.dw=96,.dh=64,.matrix=601,.output=V4L2_PIX_FMT_RGB24};
 for(unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);i++) {
  t.input=formats[i]; t.output=V4L2_PIX_FMT_RGB24;
  for(t.angle=0;t.angle<360;t.angle+=90) for(t.flip=0;t.flip<4;t.flip++) run(node,&t);
  if(!yuv(formats[i]) && formats[i]!=V4L2_PIX_FMT_ARGB444 && formats[i]!=V4L2_PIX_FMT_XRGB444 &&
 formats[i]!=V4L2_PIX_FMT_ABGR444 && formats[i]!=V4L2_PIX_FMT_XBGR444) {
   t.input=V4L2_PIX_FMT_RGB24;t.output=formats[i];
   for(t.angle=0;t.angle<360;t.angle+=90) for(t.flip=0;t.flip<4;t.flip++) run(node,&t);
  }
 }
}
static void palette_suite(const char *node)
{
 const uint32_t formats[] = {V4L2_PIX_FMT_PAL8,V4L2_PIX_FMT_RGA1_PAL1,V4L2_PIX_FMT_RGA1_PAL2,V4L2_PIX_FMT_RGA1_PAL4};
 uint8_t lut[1024],raw[64*48],expected[64*48*3];
 for(unsigned i=0;i<256;i++) {lut[4*i]=i;lut[4*i+1]=255-i;lut[4*i+2]=(i*11)&255;lut[4*i+3]=255;}
 for(unsigned f=1;f<4;f++)for(unsigned padded=0;padded<2;padded++) {
  unsigned w=padded?61:64,h=padded?47:48;
  struct test t={.sw=w,.sh=h,.dw=w,.dh=h,.matrix=601,.input=formats[f],.output=V4L2_PIX_FMT_RGB24};
  unsigned bits=palette_bits(formats[f]),mask=(1U<<bits)-1,stride=line_bytes(t.output,w);
  memset(raw,0,sizeof(raw));memset(expected,0xa5,sizeof(expected));
  for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
   unsigned index=(x+13*y+(x/3)*7)&mask;
   raw[y*line_bytes(t.input,w)+x*bits/8]|=index<<(8-bits-((x*bits)%8));
   for(unsigned c=0;c<3;c++) expected[y*stride+x*3+c]=bits==1?(index?255:0):lut[4*index+c];
  }
  struct setting setting={.id=V4L2_CID_RGA1_LUT,.data=lut,.size=sizeof(lut)};
  uint8_t *a=job(node,&t,raw,NULL,&setting,1,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"palette %u bpp %ux%u all indices and row padding",bits,w,h);
  exact(label,a,expected,sizeimage(t.output,w,h),0);free(a);
 }
}
static void mono_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGA1_PAL1,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48/8],expected[64*48*3];
 struct setting settings[]={{.id=V4L2_CID_BG_COLOR,.value=0x123456},{.id=V4L2_CID_RGA1_FG_COLOR,.value=0xabcdef}};
 for(unsigned variant=0;variant<3;variant++) {
  for(unsigned i=0;i<sizeof(raw);i++)raw[i]=variant==0?0:variant==1?255:(i*37+0x69)&255;
  for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++) {
   bool bit=(raw[y*8+x/8]>>(7-x%8))&1;uint32_t rgb=bit?0xabcdef:0x123456;
   expected[(y*64+x)*3]=rgb>>16;expected[(y*64+x)*3+1]=rgb>>8;expected[(y*64+x)*3+2]=rgb;
  }
  uint8_t *a=job(node,&t,raw,NULL,settings,2,NULL,NULL,false);
  printf("{\"mono_variant\":%u,\"first_rgb\":[%u,%u,%u]}\n",variant,a[0],a[1],a[2]);
  char label[100];snprintf(label,sizeof(label),"1bpp custom foreground/background variant %u",variant);
  exact(label,a,expected,sizeof(expected),0);free(a);
 }
}
static void rejected_stream(const char *node,struct test t,bool blend)
{
 int fd=open(node,O_RDWR|O_NONBLOCK|O_CLOEXEC);if(fd<0)die(node);configure(fd,&t);
 if(blend){struct v4l2_control c={.id=V4L2_CID_RGA1_ALPHA_MODE,.value=2};must(fd,VIDIOC_S_CTRL,&c,"alpha negative");}
 struct memory src=alloc_mmap(fd,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE),dst=alloc_mmap(fd,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
 queue(fd,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,&src,&t);queue(fd,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,&dst,&t);
 enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
 if(xioctl(fd,VIDIOC_STREAMON,&type)!=-1 || errno!=EOPNOTSUPP){fprintf(stderr,"unverified stream accepted\n");exit(1);}
 munmap(src.addr,src.len);munmap(dst.addr,dst.len);close(src.fd);close(dst.fd);close(fd);
}
static void negative_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 int fd=open(node,O_RDWR|O_NONBLOCK|O_CLOEXEC);if(fd<0)die(node);configure(fd,&t);
 for(unsigned op=2;op<=4;op++) {
  struct v4l2_control ctrl={.id=V4L2_CID_RGA1_OPERATION,.value=op};
  if(xioctl(fd,VIDIOC_S_CTRL,&ctrl)!=-1 || errno!=EINVAL) {fprintf(stderr,"absent silicon operation accepted\n");exit(1);}
 }
 struct v4l2_queryctrl rotation={.id=V4L2_CID_ROTATE};must(fd,VIDIOC_QUERYCTRL,&rotation,"query rotation");
 if(rotation.step!=1 || rotation.maximum!=359){fprintf(stderr,"invalid integer rotation range\n");exit(1);}
 struct v4l2_queryctrl alternate={.id=V4L2_CID_RGA1_BASE+24};
 if(xioctl(fd,VIDIOC_QUERYCTRL,&alternate)!=-1 || errno!=EINVAL){fprintf(stderr,"reserved diagnostic control advertised\n");exit(1);}
 alternate.id=V4L2_CID_RGA1_BASE+30;
 if(xioctl(fd,VIDIOC_QUERYCTRL,&alternate)!=-1||errno!=EINVAL){fprintf(stderr,"resource fd advertised as scalar control\n");exit(1);}
 struct v4l2_queryctrl rop={.id=V4L2_CID_RGA1_ROP};must(fd,VIDIOC_QUERYCTRL,&rop,"query ROP");
 if(rop.maximum!=65536){fprintf(stderr,"invalid ROP4 truth table range\n");exit(1);}
 struct v4l2_format pal={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};pal.fmt.pix_mp.width=64;pal.fmt.pix_mp.height=48;pal.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_PAL8;
 must(fd,VIDIOC_TRY_FMT,&pal,"PAL8 negative");if(pal.fmt.pix_mp.pixelformat==V4L2_PIX_FMT_PAL8){fprintf(stderr,"PAL8 advertised\n");exit(1);}
 close(fd);
 t.input=V4L2_PIX_FMT_RGBA32;t.output=V4L2_PIX_FMT_RGB565;rejected_stream(node,t,true);
 t.input=V4L2_PIX_FMT_RGB24;t.output=V4L2_PIX_FMT_NV12;t.matrix=709;rejected_stream(node,t,false);
 puts("{\"proof\":\"absent operations, PAL8, unsupported 16-bit alpha destination and output709 rejected before DMA\",\"pass\":true}");
}
static void ops_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*3],initial[sizeof(raw)],expected[sizeof(raw)];
 for(unsigned i=0;i<sizeof(raw);i++) {raw[i]=(i*31+i/7)&255;initial[i]=(i*17+i/11)&255;}
 uint8_t *a=job(node,&t,raw,NULL,NULL,0,NULL,NULL,true);
 exact("system-heap DMA-BUF MMU identity",a,raw,sizeof(raw),0);free(a);
 {
  struct test big=t;big.sw=big.dw=1920;big.sh=big.dh=1080;
  size_t size=sizeimage(big.input,big.sw,big.sh);
  uint8_t *fixture=malloc(size);if(!fixture)die("large fixture");
  for(size_t i=0;i<size;i++)fixture[i]=(i*31+i/193)&255;
  a=job(node,&big,fixture,NULL,NULL,0,NULL,NULL,true);
  exact("6 MiB system-heap MMU identity",a,fixture,size,0);free(a);free(fixture);
 }
 struct v4l2_rect crop={.left=8,.top=6,.width=32,.height=24},compose={.left=12,.top=10,.width=32,.height=24};
 memset(expected,0xa5,sizeof(expected));
 for(unsigned y=0;y<24;y++) memcpy(expected+((y+10)*64+12)*3,raw+((y+6)*64+8)*3,32*3);
 a=job(node,&t,raw,NULL,NULL,0,&crop,&compose,false);
 exact("crop compose including untouched borders",a,expected,sizeof(raw),0);free(a);
 struct setting fill[]={{.id=V4L2_CID_RGA1_OPERATION,.value=1},{.id=V4L2_CID_BG_COLOR,.value=0x2266aa}};
 for(unsigned i=0;i<sizeof(expected);i+=3){expected[i]=0x22;expected[i+1]=0x66;expected[i+2]=0xaa;}
 a=job(node,&t,raw,NULL,fill,2,NULL,NULL,false);exact("solid RGB fill",a,expected,sizeof(raw),0);free(a);
 for(unsigned code=0;code<256;code++) {
  /* P-independent truth tables establish all 16 two-operand Boolean functions. */
  if((code&15)!=(code>>4)) continue;
  struct setting rop={0};rop.id=V4L2_CID_RGA1_ROP;rop.value=code+1;
  for(unsigned i=0;i<sizeof(raw);i++) {
   expected[i]=0;
   for(unsigned bit=0;bit<8;bit++) expected[i]|=((code>>((((raw[i]>>bit)&1)<<1)|((initial[i]>>bit)&1)))&1)<<bit;
  }
  char label[64];snprintf(label,sizeof(label),"ROP3 P-independent truth table %02x",code);
  a=job(node,&t,raw,initial,&rop,1,NULL,NULL,false);exact(label,a,expected,sizeof(raw),0);free(a);
 }
 for(unsigned alpha=0;alpha<256;alpha+=51) {
  struct setting blend[]={{.id=V4L2_CID_RGA1_ALPHA_MODE,.value=1},{.id=V4L2_CID_ALPHA_COMPONENT,.value=alpha}};
  for(unsigned i=0;i<sizeof(raw);i++) expected[i]=(raw[i]*(alpha+1)+initial[i]*(255-alpha))>>8;
  char label[64];snprintf(label,sizeof(label),"global alpha %u",alpha);
  a=job(node,&t,raw,initial,blend,2,NULL,NULL,false);exact(label,a,expected,sizeof(raw),1);free(a);
 }
 for(unsigned invert=0;invert<2;invert++) {
  struct setting key[]={{.id=V4L2_CID_RGA1_COLORKEY,.value=invert+1},{.id=V4L2_CID_RGA1_KEY_MIN,.value=0x202020},{.id=V4L2_CID_RGA1_KEY_MAX,.value=0xc0c0c0}};
  for(unsigned i=0;i<sizeof(raw);i+=3) {
   bool inside=raw[i]>=32&&raw[i]<=192&&raw[i+1]>=32&&raw[i+1]<=192&&raw[i+2]>=32&&raw[i+2]<=192;
   memcpy(expected+i,inside!=!!invert?initial+i:raw+i,3);
  }
  a=job(node,&t,raw,initial,key,3,NULL,NULL,false);exact(invert?"inverted RGB color key":"RGB color key",a,expected,sizeof(raw),0);free(a);
 }
}
static void alpha_opaque_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGBA32,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*4],bg[64*48*3],expected[sizeof(bg)];
 const unsigned alphas[]={0,1,63,127,128,191,254,255};
 for(unsigned i=0;i<64*48;i++) {
  unsigned sa=alphas[i%8];
  raw[4*i]=sa*3/4;raw[4*i+1]=sa/2;raw[4*i+2]=sa/4;raw[4*i+3]=sa;
  bg[3*i]=(i*17)&255;bg[3*i+1]=(i*11)&255;bg[3*i+2]=(i*31)&255;
 }
 for(unsigned mode=1;mode<=3;mode++)for(unsigned pd=0;pd<=12;pd++) {
  unsigned global=mode==2?255:153;
  struct setting settings[]={{.id=V4L2_CID_RGA1_ALPHA_MODE,.value=mode},{.id=V4L2_CID_RGA1_PORTER_DUFF,.value=pd},{.id=V4L2_CID_ALPHA_COMPONENT,.value=global}};
  for(unsigned i=0;i<64*48;i++) {
   double sa=mode==1?global/255.0:raw[4*i+3]/255.0*(mode==3?global/255.0:1),da=mode==2?1:global/255.0,fs=sa,fd=1-sa;
   switch(pd) {
   case 1:fs=fd=0;break;case 2:fs=1;fd=0;break;case 3:fs=0;fd=1;break;
   case 4:fs=1;fd=1-sa;break;case 5:fs=1-da;fd=1;break;
   case 6:fs=da;fd=0;break;case 7:fs=0;fd=sa;break;
   case 8:fs=1-da;fd=0;break;case 9:fs=0;fd=1-sa;break;
   case 10:fs=da;fd=1-sa;break;case 11:fs=1-da;fd=sa;break;
   case 12:fs=1-da;fd=1-sa;break;
   }
   for(unsigned c=0;c<3;c++)expected[3*i+c]=byte(raw[4*i+c]*fs+bg[3*i+c]*fd);
  }
  uint8_t *a=job(node,&t,raw,bg,settings,3,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"opaque destination alpha-mode %u PD %u",mode,pd);
  exact(label,a,expected,sizeof(bg),2);free(a);
 }
}
static void geometry_suite(const char *node)
{
 struct test t={.sw=64,.sh=64,.dw=64,.dh=64,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*64*3],expected[sizeof(raw)],bg[sizeof(raw)];
 for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(i%3==0?34:i%3==1?102:170);
 memset(bg,0xa5,sizeof(bg));
 for(unsigned mode=0;mode<3;mode++) {
  struct setting setting={.id=V4L2_CID_RGA1_SAMPLING,.value=mode};
  uint8_t *a=job(node,&t,raw,NULL,&setting,1,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"sampling %u identity constant field",mode);
  exact(label,a,raw,sizeof(raw),0);free(a);
 }
 for(unsigned mode=0;mode<3;mode++) {
  struct test scaled=t;scaled.dw=96;scaled.dh=80;
  uint8_t expected_scaled[96*80*3];for(unsigned i=0;i<sizeof(expected_scaled);i++)expected_scaled[i]=raw[i%3];
  struct setting setting={.id=V4L2_CID_RGA1_SAMPLING,.value=mode};
  uint8_t *a=job(node,&scaled,raw,NULL,&setting,1,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"sampling %u 64x64 to 96x80 constant field all borders",mode);
  exact(label,a,expected_scaled,sizeof(expected_scaled),0);free(a);
 }
 for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(i%3==0?34:i%3==1?102:170);
 t.angle=0;
 uint8_t pattern[1024]={0};const uint8_t colors[4][3]={{32,64,96},{48,80,112},{64,96,128},{80,112,144}};
 memcpy(pattern,colors,sizeof(colors));
 struct setting fill[]={{.id=V4L2_CID_RGA1_OPERATION,.value=5},{.id=V4L2_CID_RGA1_PATTERN_SIZE,.value=(2<<16)|2},{.id=V4L2_CID_RGA1_PATTERN,.data=pattern,.size=sizeof(pattern)}};
 for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)memcpy(expected+(y*64+x)*3,colors[(y%2)*2+x%2],3);
 uint8_t *a=job(node,&t,raw,NULL,fill,3,NULL,NULL,false);exact("2x2 RGB pattern fill",a,expected,sizeof(raw),0);free(a);
}
static void special_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*3],expected[sizeof(raw)],bg[sizeof(raw)];
 memset(bg,0xa5,sizeof(bg));
 memset(raw,0,sizeof(raw));
 uint8_t *a;
 int32_t gradient[]={0,0,0,256,256,0,128,128};
 struct setting fill[]={{.id=V4L2_CID_RGA1_OPERATION,.value=1},{.id=V4L2_CID_BG_COLOR,.value=0x202020},{.id=V4L2_CID_RGA1_GRADIENT,.data=gradient,.size=sizeof(gradient)}};
 for(unsigned y0=0;y0<48;y0++) for(unsigned x0=0;x0<64;x0++) {
  expected[(y0*64+x0)*3]=32+(x0+y0)/2;expected[(y0*64+x0)*3+1]=32+x0;expected[(y0*64+x0)*3+2]=32+y0;
 }
 a=job(node,&t,raw,NULL,fill,3,NULL,NULL,false);exact("signed 8.8 gradient fill",a,expected,sizeof(raw),1);free(a);
 for(unsigned i=0;i<sizeof(raw);i++) raw[i]=20+i%100;
 struct setting fading={.id=V4L2_CID_RGA1_FADING,.value=0x101820};
 for(unsigned i=0;i<sizeof(raw);i++) expected[i]=raw[i]+(i%3==0?16:i%3==1?24:32);
 a=job(node,&t,raw,NULL,&fading,1,NULL,NULL,false);exact("RGB additive fading",a,expected,sizeof(raw),1);free(a);
 t.input=V4L2_PIX_FMT_NV12;generate(raw,&t);
 uint8_t *normal=job(node,&t,raw,NULL,NULL,0,NULL,NULL,false);
 struct setting alias={.id=V4L2_CID_RGA1_CSC_ALIAS,.value=1};
 a=job(node,&t,raw,NULL,&alias,1,NULL,NULL,false);exact("CSC selector3 equals BT601 limited selector0",a,normal,sizeof(raw),0);
 /* A competing 709-full model must disagree substantially on the same pixels. */
 struct test full709=t;full709.matrix=709;full709.full=1;reference(raw,expected,&full709);
 unsigned differences=0;for(unsigned i=0;i<sizeof(raw);i++) if(abs((int)a[i]-expected[i])>6) differences++;
 printf("{\"proof\":\"CSC selector3 rejects competing BT709 full model\",\"different_channels\":%u,\"pass\":%s}\n",differences,differences>sizeof(raw)/10?"true":"false");
 if(differences<=sizeof(raw)/10) failures++;
 free(a);free(normal);
}
static void zero_key_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGBA32,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*4],expected[64*48*3];
 for(unsigned i=0;i<64*48;i++) {
  unsigned pattern=i%4;
  raw[4*i]=pattern>1?34:0;raw[4*i+1]=pattern>1?102:0;raw[4*i+2]=pattern>1?170:0;raw[4*i+3]=pattern%2?255:0;
  memcpy(expected+3*i,raw+4*i,3);
  if(pattern==1)expected[3*i+1]=1;
 }
 struct setting zero={.id=V4L2_CID_RGA1_ZERO_KEY,.value=1};
 uint8_t *a=job(node,&t,raw,NULL,&zero,1,NULL,NULL,false);
 exact("alpha-zero key: opaque-black collision avoidance preserves other RGB",a,expected,sizeof(expected),0);free(a);
}
static void fill_rect_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*3]={0},expected[sizeof(raw)];
 const struct v4l2_rect rects[]={{7,9,1,1},{11,13,3,5},{63,47,1,1}};
 struct setting fill[]={{.id=V4L2_CID_RGA1_OPERATION,.value=1},{.id=V4L2_CID_BG_COLOR,.value=0x2266aa}};
 for(unsigned n=0;n<3;n++) {
  memset(expected,0xa5,sizeof(expected));
  for(unsigned y=0;y<rects[n].height;y++)for(unsigned x=0;x<rects[n].width;x++) {
   unsigned i=((y+rects[n].top)*64+x+rects[n].left)*3;expected[i]=34;expected[i+1]=102;expected[i+2]=170;
  }
  uint8_t *a=job(node,&t,raw,NULL,fill,2,NULL,&rects[n],false);
  char label[100];snprintf(label,sizeof(label),"rectangle fill %ux%u at %d,%d all untouched borders",rects[n].width,rects[n].height,rects[n].left,rects[n].top);
  exact(label,a,expected,sizeof(raw),0);free(a);
 }
}
static void fill_alpha_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGBA32,.output=V4L2_PIX_FMT_RGBA32};
 uint8_t raw[64*48*4]={0},expected[sizeof(raw)];
 const unsigned alphas[]={0,1,63,127,128,191,254,255};
 for(unsigned n=0;n<8;n++) {
  for(unsigned i=0;i<64*48;i++){expected[4*i]=34;expected[4*i+1]=102;expected[4*i+2]=170;expected[4*i+3]=alphas[n];}
  struct setting settings[]={{.id=V4L2_CID_RGA1_OPERATION,.value=1},{.id=V4L2_CID_BG_COLOR,.value=0x2266aa},{.id=V4L2_CID_ALPHA_COMPONENT,.value=alphas[n]}};
  uint8_t *a=job(node,&t,raw,NULL,settings,3,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"RGBA fill alpha %u",alphas[n]);exact(label,a,expected,sizeof(raw),0);free(a);
 }
}
static void palette_alpha_suite(const char *node)
{
 const uint32_t formats[]={V4L2_PIX_FMT_RGA1_PAL2,V4L2_PIX_FMT_RGA1_PAL4};
 uint8_t lut[1024]={0},raw[64*48],expected[64*48*4];
 const unsigned alphas[]={0,1,63,127,128,191,254,255};
 for(unsigned i=0;i<256;i++){lut[4*i]=i;lut[4*i+1]=255-i;lut[4*i+2]=i*11;lut[4*i+3]=alphas[i%8];}
 for(unsigned f=0;f<2;f++)for(unsigned mode=0;mode<2;mode++) {
  struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=formats[f],.output=V4L2_PIX_FMT_RGBA32};
  unsigned bits=palette_bits(t.input),mask=(1U<<bits)-1;memset(raw,0,sizeof(raw));
  for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++) {
   unsigned index=(x+13*y)&mask;raw[y*line_bytes(t.input,64)+x*bits/8]|=index<<(8-bits-(x*bits)%8);
   memcpy(expected+(y*64+x)*4,lut+4*index,4);
   if(mode)expected[(y*64+x)*4+3]=lut[4*index+3]&1?255:0;
  }
  struct setting settings[]={{.id=V4L2_CID_RGA1_LUT,.data=lut,.size=sizeof(lut)},{.id=V4L2_CID_RGA1_PALETTE_ALPHA,.value=mode}};
  uint8_t *a=job(node,&t,raw,NULL,settings,2,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"palette %u bpp LUT alpha %u-bit",bits,mode?1:8);
  exact(label,a,expected,sizeof(expected),0);free(a);
 }
}
static void bitfield_suite(const char *node)
{
 const uint32_t fmts[]={V4L2_PIX_FMT_RGBA555,V4L2_PIX_FMT_BGRA555,V4L2_PIX_FMT_ARGB444,V4L2_PIX_FMT_ABGR444,V4L2_PIX_FMT_RGBA444,V4L2_PIX_FMT_BGRA444};
 uint16_t raw[65536];for(unsigned i=0;i<65536;i++)raw[i]=i;
 for(unsigned f=0;f<sizeof(fmts)/sizeof(fmts[0]);f++) {
  struct test t={.sw=256,.sh=256,.dw=256,.dh=256,.matrix=601,.input=fmts[f],.output=V4L2_PIX_FMT_RGBA32};
  uint8_t *a=job(node,&t,(uint8_t*)raw,NULL,NULL,0,NULL,NULL,false);
  printf("{\"bitfield\":\"%.4s\",\"zero\":[%u,%u,%u,%u],\"one_bits\":[",(char*)&t.input,a[0],a[1],a[2],a[3]);
  for(unsigned bit=0;bit<16;bit++) {unsigned k=4*(1U<<bit);printf("%s[%u,%u,%u,%u]",bit?",":"",a[k],a[k+1],a[k+2],a[k+3]);}
  printf("]}\n");fflush(stdout);
  uint8_t *e=malloc(65536*4);if(!e)die("bitfield reference");
  struct rgb_layout l=layout(t.input);
  for(unsigned v=0;v<65536;v++)for(unsigned c=0;c<4;c++) {
   unsigned component=(v>>l.shifts[c])&((1U<<l.bits[c])-1);
   e[4*v+c]=c==3&&l.bits[c]==1?component*255:component<<(8-l.bits[c]);
  }
  char label[80];snprintf(label,sizeof(label),"%.4s exhaustive 65536 values including alpha",(char*)&t.input);
  exact(label,a,e,65536*4,0);free(e);free(a);
 }
}
static void rgb_to_yuv(const double rgb[3],const struct test *t,double out[3])
{
 double kr=t->matrix==709?.2126:.299,kb=t->matrix==709?.0722:.114;
 double y=kr*rgb[0]+(1-kr-kb)*rgb[1]+kb*rgb[2];
 out[0]=t->full?y:16+y*219/255;
 out[1]=128+(rgb[2]-y)/(2*(1-kb))*(t->full?1:224.0/255);
 out[2]=128+(rgb[0]-y)/(2*(1-kr))*(t->full?1:224.0/255);
}
static void yuv_suite(const char *node)
{
 const uint32_t formats[]={V4L2_PIX_FMT_NV12,V4L2_PIX_FMT_NV21,V4L2_PIX_FMT_NV16,V4L2_PIX_FMT_NV61};
 const double colors[][3]={{0,0,0},{255,255,255},{255,0,0},{0,255,0},{0,0,255},{50,100,150},{180,80,30},{17,239,96}};
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*3],expected[sizeof(raw)];
 for(unsigned f=0;f<4;f++) for(unsigned mode=0;mode<2;mode++) for(unsigned color=0;color<8;color++) {
  t.output=formats[f];t.matrix=mode==2?709:601;t.full=mode==1;
  double yc[3];rgb_to_yuv(colors[color],&t,yc);
  memset(expected,0,sizeof(expected));
  for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++) {
   rgb_store(raw+(y*64+x)*3,t.input,colors[color]);
   expected[y*64+x]=byte(yc[0]);
   expected[chroma_offset(t.output,64,48,x,y,0)]=byte(yc[1]);
   expected[chroma_offset(t.output,64,48,x,y,1)]=byte(yc[2]);
  }
  uint8_t *a=job(node,&t,raw,NULL,NULL,0,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"RGB to %.4s CSC %u %s color %u",(char*)&t.output,t.matrix,t.full?"full":"limited",color);
  printf("{\"yuv_samples\":[%u,%u,%u,%u,%u,%u]}\n",mode,color,f,a[0],a[64*48],a[64*48+1]);
  exact(label,a,expected,sizeimage(t.output,64,48),2);free(a);
 }
 t.matrix=601;t.full=0;
 for(unsigned in=0;in<4;in++)for(unsigned out=0;out<4;out++) {
  t.input=formats[in];t.output=formats[out];memset(raw,0,sizeof(raw));memset(expected,0,sizeof(expected));
  for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++) {
   raw[y*64+x]=expected[y*64+x]=48;
   raw[chroma_offset(t.input,64,48,x,y,0)]=108;raw[chroma_offset(t.input,64,48,x,y,1)]=180;
   expected[chroma_offset(t.output,64,48,x,y,0)]=108;expected[chroma_offset(t.output,64,48,x,y,1)]=180;
  }
  uint8_t *a=job(node,&t,raw,NULL,NULL,0,NULL,NULL,false);
  char label[80];snprintf(label,sizeof(label),"YUV bypass %.4s to %.4s",(char*)&t.input,(char*)&t.output);
  exact(label,a,expected,sizeimage(t.output,64,48),0);free(a);
 }
}
static void dither_suite(const char *node)
{
 struct test t={.sw=16,.sh=16,.dw=16,.dh=16,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB565};
 uint8_t raw[16*16*3];struct setting dither={.id=V4L2_CID_RGA1_DITHER,.value=1};
 unsigned errors=0,changed=0;double worst_mean=0;
 for(unsigned value=0;value<256;value++) {
  memset(raw,value,sizeof(raw));uint8_t *a=job(node,&t,raw,NULL,&dither,1,NULL,NULL,false);
  double sums[3]={0};
  for(unsigned pixel=0;pixel<256;pixel++) {
   double rgb[3];rgb_read(a+pixel*2,t.output,rgb);
   for(unsigned c=0;c<3;c++) {
    unsigned step=c==1?4:8,low=value&~(step-1),high=low+step>256-step?256-step:low+step;
    if(rgb[c]!=low&&rgb[c]!=high)errors++;
    if(rgb[c]!=low)changed++;
    sums[c]+=rgb[c];
   }
  }
  for(unsigned c=0;c<3;c++) {
   double target=fmin(value,c==1?252:248),err=fabs(sums[c]/256-target);
   if(err>worst_mean)worst_mean=err;
   if(err>2)errors++;
  }
  free(a);
 }
 printf("{\"proof\":\"ordered RGB565 dithering, all 256 levels all pixels\",\"errors\":%u,\"changed_components\":%u,\"worst_spatial_mean_error\":%.4f,\"pass\":%s}\n",errors,changed,worst_mean,!errors&&changed?"true":"false");
 if(errors||!changed)failures++;
}
static void rop_suite(const char *node)
{
 struct test t={.sw=64,.sh=48,.dw=64,.dh=48,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t raw[64*48*3],bg[sizeof(raw)],expected[sizeof(raw)],pattern[1024]={0};
 const uint8_t p[3]={0x69,0xa5,0x3c};memcpy(pattern,p,3);
 for(unsigned i=0;i<sizeof(raw);i++){raw[i]=(i*31+i/7)&255;bg[i]=(i*17+i/11)&255;}
 for(unsigned code=0;code<256;code++) {
  for(unsigned i=0;i<sizeof(raw);i++) {
   expected[i]=0;
   for(unsigned bit=0;bit<8;bit++) {
    unsigned index=(((p[i%3]>>bit)&1)<<2)|(((raw[i]>>bit)&1)<<1)|((bg[i]>>bit)&1);
    expected[i]|=((code>>index)&1)<<bit;
   }
  }
  struct setting controls[]={{.id=V4L2_CID_RGA1_ROP,.value=code+1},{.id=V4L2_CID_RGA1_PATTERN,.data=pattern,.size=sizeof(pattern)}};
  uint8_t *a=job(node,&t,raw,bg,controls,2,NULL,NULL,false);
  char label[80];snprintf(label,sizeof(label),"full ROP3 truth table %02x, RGB pattern",code);
  exact(label,a,expected,sizeof(raw),0);free(a);
 }
}

static void rejected_pattern(const char *node,const struct test *t,unsigned w,unsigned h)
{
 int fd=open(node,O_RDWR|O_NONBLOCK|O_CLOEXEC);if(fd<0)die(node);configure(fd,t);
 struct v4l2_control c={.id=V4L2_CID_RGA1_OPERATION,.value=5};must(fd,VIDIOC_S_CTRL,&c,"pattern negative");c.id=V4L2_CID_RGA1_PATTERN_SIZE;c.value=(h<<16)|w;
 if(xioctl(fd,VIDIOC_S_CTRL,&c)==-1){if(errno!=ERANGE && errno!=EINVAL)die("pattern bound control");close(fd);return;}
 struct memory src=alloc_mmap(fd,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE),dst=alloc_mmap(fd,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
 queue(fd,V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,&src,t);queue(fd,V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,&dst,t);
 enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
 if(xioctl(fd,VIDIOC_STREAMON,&type)!=-1 || errno!=EINVAL){fprintf(stderr,"corrupt pattern RAM entry exposed\n");exit(1);}
 munmap(src.addr,src.len);munmap(dst.addr,dst.len);close(src.fd);close(dst.fd);close(fd);
}
static void pattern_suite(const char *node)
{
 struct test t={.sw=256,.sh=256,.dw=256,.dh=256,.matrix=601,.input=V4L2_PIX_FMT_RGB24,.output=V4L2_PIX_FMT_RGB24};
 uint8_t *raw=calloc(1,256*256*3),*expected=malloc(256*256*3),pattern[1024]={0};if(!raw||!expected)die("pattern fixtures");
 for(unsigned i=0;i<256;i++){pattern[3*i]=i;pattern[3*i+1]=255-i;pattern[3*i+2]=i*11;}
 const unsigned counts[]={1,2,3,4,5,7,8,15,16,31,32,63,64,127,128,255,256};
 for(unsigned n=0;n<sizeof(counts)/sizeof(counts[0])+11;n++)for(unsigned transpose=0;transpose<2;transpose++) {
  unsigned w,h,total=sizeof(counts)/sizeof(counts[0]);
  const unsigned extra[][2]={{3,85},{5,51},{15,17},{15,16}};
  if(n<total){w=counts[n];h=1;}
  else if(n<total+7){w=1U<<(n-total+1);h=256/w;}
  else {w=extra[n-total-7][0];h=extra[n-total-7][1];}
  if(transpose){unsigned tmp=w;w=h;h=tmp;}
  if(w*h>255){rejected_pattern(node,&t,w,h);printf("{\"proof\":\"pattern %ux%u outside verified RAM rejected before DMA\",\"pass\":true}\n",w,h);continue;}
  struct setting fill[]={{.id=V4L2_CID_RGA1_OPERATION,.value=5},{.id=V4L2_CID_RGA1_PATTERN_SIZE,.value=(h<<16)|w},{.id=V4L2_CID_RGA1_PATTERN,.data=pattern,.size=sizeof(pattern)}};
  for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++)memcpy(expected+(y*256+x)*3,pattern+((y%h)*w+x%w)*3,3);
  uint8_t *a=job(node,&t,raw,NULL,fill,3,NULL,NULL,false);
  char label[100];snprintf(label,sizeof(label),"pattern %ux%u repeated across 256x256 all entries and borders",w,h);exact(label,a,expected,256*256*3,0);free(a);
 }
 free(raw);free(expected);
}
#ifndef RGA1_PROBE_MAIN
#define RGA1_PROBE_MAIN main
#endif
int RGA1_PROBE_MAIN(int argc,char **argv)
{
 if(argc!=3){fprintf(stderr,"usage: %s NODE MODE\n",argv[0]);return 2;}
 if(!strcmp(argv[2],"--565"))probe565(argv[1]);
 else if(!strcmp(argv[2],"--formats"))format_suite(argv[1]);
 else if(!strcmp(argv[2],"--ops"))ops_suite(argv[1]);
 else if(!strcmp(argv[2],"--palette"))palette_suite(argv[1]);
 else if(!strcmp(argv[2],"--special"))special_suite(argv[1]);
 else if(!strcmp(argv[2],"--bits"))bitfield_suite(argv[1]);
 else if(!strcmp(argv[2],"--fill-rect"))fill_rect_suite(argv[1]);
 else if(!strcmp(argv[2],"--fill-alpha"))fill_alpha_suite(argv[1]);
 else if(!strcmp(argv[2],"--palette-alpha"))palette_alpha_suite(argv[1]);
 else if(!strcmp(argv[2],"--mono"))mono_suite(argv[1]);
 else if(!strcmp(argv[2],"--negative"))negative_suite(argv[1]);
 else if(!strcmp(argv[2],"--zero-key"))zero_key_suite(argv[1]);
 else if(!strcmp(argv[2],"--alpha-opaque"))alpha_opaque_suite(argv[1]);
 else if(!strcmp(argv[2],"--pattern"))pattern_suite(argv[1]);
 else if(!strcmp(argv[2],"--geometry"))geometry_suite(argv[1]);
 else if(!strcmp(argv[2],"--yuv"))yuv_suite(argv[1]);
 else if(!strcmp(argv[2],"--dither"))dither_suite(argv[1]);
 else if(!strcmp(argv[2],"--rop"))rop_suite(argv[1]);
 else return 2;
 return failures?1:0;
}
