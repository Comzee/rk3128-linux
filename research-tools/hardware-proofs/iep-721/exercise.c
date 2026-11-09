/* SPDX-License-Identifier: MIT
 * Finite generated-fixture IEP experiment. All inputs are synthetic files.
 * Reuses checked ioctls, DMA-BUF synchronization and bounded dequeue helpers.
 * Usage: exercise DEVICE WIDTH HEIGHT SRC DST INPUT OUTPUT OPTIONS
 * OPTIONS: comma-separated key=value (see option() below), '-' for defaults.
 */
#define main iep_standard_main
#include "../../../tools/iep-test.c"
#undef main
#include <linux/dma-heap.h>

#define V4L2_PIX_FMT_RK_IEP_BGR565 v4l2_fourcc('R', 'K', 'B', '5')
#define V4L2_PIX_FMT_RK_IEP_YVU422 v4l2_fourcc('R', 'K', 'V', '6')
struct layout { const char *name; uint32_t fourcc; unsigned bpp, sub; bool planar; };
static const struct layout layouts[] = {
    { "nv12", V4L2_PIX_FMT_NV12, 1, 2, false },
    { "nv21", V4L2_PIX_FMT_NV21, 1, 2, false },
    { "nv16", V4L2_PIX_FMT_NV16, 1, 1, false },
    { "nv61", V4L2_PIX_FMT_NV61, 1, 1, false },
    { "yu12", V4L2_PIX_FMT_YUV420, 1, 2, true },
    { "yv12", V4L2_PIX_FMT_YVU420, 1, 2, true },
    { "422p", V4L2_PIX_FMT_YUV422P, 1, 1, true },
    { "bgra", V4L2_PIX_FMT_ABGR32, 4, 0, false },
    { "rgba", V4L2_PIX_FMT_RGBA32, 4, 0, false },
    { "abgr", V4L2_PIX_FMT_BGRA32, 4, 0, false },
    { "argb", V4L2_PIX_FMT_ARGB32, 4, 0, false },
    { "rgb565", V4L2_PIX_FMT_RGB565, 2, 0, false },
    { "bgr565", V4L2_PIX_FMT_RK_IEP_BGR565, 2, 0, false },
    { "yv16", V4L2_PIX_FMT_RK_IEP_YVU422, 1, 1, true },
};
struct options { unsigned field, dei, filter, radius, amount, curve, color, coef, order,
                         weights, distance, basis, csc, alpha, ei, hf, threshold, hflip, vflip, pattern, dither, probe, aliases, reset, reject, abort_job, one_capture, scale, dw, dh, heap, alias, spad, dpad, alignment; };
static const struct layout *layout(const char *name)
{
    for (unsigned i = 0; i < sizeof(layouts)/sizeof(layouts[0]); i++)
        if (!strcmp(name, layouts[i].name)) return &layouts[i];
    fputs("unknown format\n", stderr); exit(2);
}
static void option(struct options *o, char *key, const char *val)
{
    unsigned n = number(val, 0, UINT_MAX);
#define OPT(k, member, limit) if (!strcmp(key,k)) { if (n > limit) exit(2); o->member=n; return; }
    OPT("field",field,2) OPT("dei",dei,2) OPT("filter",filter,3)
    OPT("radius",radius,3) OPT("amount",amount,255) OPT("curve",curve,4)
    OPT("color",color,1) OPT("coef",coef,127) OPT("order",order,1)
    OPT("distance",distance,3) OPT("basis",basis,256) OPT("weights",weights,70) OPT("csc",csc,3) OPT("alpha",alpha,255)
    OPT("heap",heap,1) OPT("alias",alias,3) OPT("spad",spad,1024) OPT("dpad",dpad,1024) OPT("align",alignment,256) OPT("scale",scale,1) OPT("dw",dw,1920) OPT("dh",dh,1088) OPT("reset",reset,1) OPT("reject",reject,1) OPT("abort",abort_job,1) OPT("onecap",one_capture,1) OPT("aliases",aliases,1) OPT("probe",probe,3) OPT("hflip",hflip,1) OPT("vflip",vflip,1) OPT("pattern",pattern,3) OPT("dither",dither,1)
    OPT("ei",ei,31) OPT("hf",hf,127) OPT("threshold",threshold,255)
#undef OPT
    fputs("unknown option\n", stderr); exit(2);
}
static void scalar(int fd, const char *name, unsigned value)
{
    uint32_t id = control_by_name(fd, name);
    set_control(fd, id, value);
    struct v4l2_control c = { .id = id };
    checked(fd, VIDIOC_G_CTRL, &c, "G_CTRL");
    if (c.value != (int)value) { fprintf(stderr,"clamped %s to %d\n",name,c.value); exit(1); }
}
static void table(int fd, const char *name, uint8_t *p, size_t size)
{
    uint32_t id=control_by_name(fd,name);
    struct v4l2_query_ext_ctrl query={ .id=id };
    checked(fd,VIDIOC_QUERY_EXT_CTRL,&query,"QUERY_EXT_CTRL table");
    if (query.elem_size!=1 || query.elems>size) { fputs("unexpected table size\n",stderr);exit(1); }
    struct v4l2_ext_control c = { .id=id, .size=query.elems, .p_u8=p };
    struct v4l2_ext_controls cs = { .count=1, .controls=&c };
    checked(fd, VIDIOC_S_EXT_CTRLS, &cs, "S_EXT_CTRLS table");
}
static void configure(int fd, struct options *o)
{
    if (o->scale) scalar(fd,"Research scaler test",o->scale);
    scalar(fd,"Spatial filter",o->filter);
    scalar(fd,"Spatial radius code",o->radius);
    scalar(fd,"Spatial threshold",o->threshold);
    set_control(fd,V4L2_CID_SHARPNESS,o->amount);
    scalar(fd,"RGB mapping enable",!!o->curve);
    scalar(fd,"RGB color enable",o->color);
    scalar(fd,"RGB color coefficient",o->coef);
    scalar(fd,"RGB DDE before mapping",o->order);
    scalar(fd,"Output alpha",o->alpha);
    scalar(fd,"Deinterlace mode",o->dei);
    if (o->hflip) set_control(fd,V4L2_CID_HFLIP,o->hflip);
    if (o->vflip) set_control(fd,V4L2_CID_VFLIP,o->vflip);
    scalar(fd,"Test Pattern",o->pattern);
    scalar(fd,"RGB565 dither up",o->dither);
    if (o->reset) scalar(fd,"Research soft reset",o->reset);
    if (o->probe) scalar(fd,"Research feature register probe",o->probe);
    /* Research-only CID: QUERY_EXT_CTRL truncates this diagnostic name. */
    if (o->aliases) set_control(fd,V4L2_CID_USER_BASE+0x1240+20,o->aliases);
    uint8_t solid[3]={96,112,144};table(fd,"Solid Y U V",solid,sizeof(solid));
    scalar(fd,"Deinterlace EI mode",o->ei & 1);
    scalar(fd,"Deinterlace EI smoothing",(o->ei >> 1) & 1);
    scalar(fd,"Deinterlace EI selection",(o->ei >> 2) & 1);
    scalar(fd,"Deinterlace EI radius",o->ei >> 3);
    scalar(fd,"Deinterlace high frequency",!!o->hf);
    scalar(fd,"Deinterlace HF factor",o->hf);
    uint8_t cg[768], weights[512];
    for (unsigned i=0;i<768;i++) {
        unsigned x=i%256, ch=i/256;
        cg[i]=o->curve==2 ? 255-x : o->curve==3 ? x*x/255 :
              o->curve==4 ? (ch==0 ? x/2 : ch==1 ? 255-x : x*x/255) : x;
    }
    table(fd,"RGB mapping B G R",cg,sizeof(cg));
    for (unsigned i=0;i<512;i++) {
        if (o->weights<=63) weights[i]=o->weights;
        else if (o->weights==64) weights[i]=i<337 ? 1 : 0; /* round 3 baseline */
        else if (o->weights==65) weights[i]=i<256 ? 1 : i==296 ? 63 : 1;
        else if (o->weights==66) weights[i]=i<256 ? 1 : (i-256)%16==0 ? 1 : 0;
        else if (o->weights==67) weights[i]=i<256 ? 1 : (i-256)%16<5 ? 1 : 0;
        else if (o->weights==68) weights[i]=i<256 ? 1 : (i-256)%16<7 ? 1 : 0;
        else if (o->weights==69) weights[i]=i<256 ? 1 : (i-256)%32<5 ? 1 : 0;
        else weights[i]=i<256 ? 1 : (i-256)%32<9 ? 1 : 0;
    }
    for (unsigned i=256;i<512;i++) {
        if (o->distance==1) weights[i]=i<337 ? 1 : 0;
        if (o->distance==2) weights[i]=i<337 ? 63 : 0;
        if (o->distance==3) weights[i]=63;
        if (o->basis && i==255+o->basis) weights[i]=63;
    }
    table(fd,"Spatial DDE weights",weights,sizeof(weights));
}
static struct image setfmt(int fd, unsigned q, unsigned w, unsigned h,
                           const struct layout *l, struct options *o)
{
    struct v4l2_format f = { .type=type(q) };
    enum v4l2_field fld = q || !o->field ? V4L2_FIELD_NONE :
                         o->field==1 ? V4L2_FIELD_INTERLACED_TB : V4L2_FIELD_INTERLACED_BT;
    f.fmt.pix=(struct v4l2_pix_format) {
        .width=w,.height=h,.pixelformat=l->fourcc,.field=fld,
        .bytesperline=w*l->bpp+(q ? o->dpad : o->spad),.priv=V4L2_PIX_FMT_PRIV_MAGIC,
        .colorspace=o->csc&2 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M,
        .ycbcr_enc=o->csc&2 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601,
        .quantization=o->csc&1 ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE,
        .xfer_func=V4L2_XFER_FUNC_709,
    };
    if (o->alignment)
        f.fmt.pix.bytesperline=(f.fmt.pix.bytesperline+o->alignment-1)/o->alignment*o->alignment;
    checked(fd,VIDIOC_S_FMT,&f,"S_FMT experiment");
    if (f.fmt.pix.pixelformat!=l->fourcc || f.fmt.pix.field!=fld ||
        f.fmt.pix.width!=w || f.fmt.pix.height!=h || f.fmt.pix.bytesperline<w*l->bpp) {
        fputs("unexpected format\n",stderr);exit(1);
    }
    size_t size=f.fmt.pix.bytesperline*h;
    if (l->sub) size+=size/l->sub;
    if ((!o->scale && f.fmt.pix.sizeimage!=size) || f.fmt.pix.sizeimage<size) { fputs("bad sizeimage\n",stderr);exit(1); }
    return (struct image) { .width=w,.height=h,.stride=f.fmt.pix.bytesperline,.size=f.fmt.pix.sizeimage };
}
static void raw(struct image *im, const struct layout *l, FILE *f, bool writing)
{
    unsigned off=0;
    for (unsigned plane=0;plane<(l->sub ? l->planar ? 3U : 2U : 1U);plane++) {
        unsigned rows=plane ? im->height/l->sub : im->height;
        unsigned cols=im->width*l->bpp/(plane && l->planar ? 2 : 1);
        unsigned pitch=im->stride/(plane && l->planar ? 2 : 1);
        for (unsigned y=0;y<rows;y++) {
            size_t n=writing ? fwrite(im->p+off+y*pitch,1,cols,f) :
                               fread(im->p+off+y*pitch,1,cols,f);
            if (n!=cols) { fputs("short raw IO\n",stderr);exit(1); }
        }
        off+=rows*pitch;
    }
}
int main(int argc, char **argv)
{
    if (argc!=9) { fputs("exercise DEVICE WIDTH HEIGHT SRC DST INPUT OUTPUT OPTIONS\n",stderr);return 2; }
    unsigned w=number(argv[2],8,1920),h=number(argv[3],16,1088);
    if (w%4 || h%4) return 2;
    const struct layout *ls[2]={layout(argv[4]),layout(argv[5])};
    struct options o={ .amount=8,.radius=1,.coef=32,.alpha=255,.weights=1,.threshold=255,.dither=1,.spad=16,.dpad=32 };
    char *opts=strdup(argv[8]),*state=NULL;
    if (!opts) die("strdup");
    if (strcmp(opts,"-")) for (char *kv=strtok_r(opts,",",&state);kv;kv=strtok_r(NULL,",",&state)) {
        char *eq=strchr(kv,'='); if (!eq) return 2; *eq++=0; option(&o,kv,eq);
    }
    free(opts);
    if (o.dei && !o.field) return 2;
    unsigned count[2]={ o.dei ? 3 : 1, o.dei==2 ? 4 : o.dei ? 2 : 1 };
    if (o.one_capture) count[1]=1;
    int fd=open(argv[1],O_RDWR|O_NONBLOCK|O_CLOEXEC),owner=open(argv[1],O_RDWR|O_CLOEXEC);
    if (fd<0 || owner<0) die("open");
    struct v4l2_capability cap={0}; checked(fd,VIDIOC_QUERYCAP,&cap,"QUERYCAP");
    if (strcmp((char *)cap.driver,"rockchip-iep")) return 2;
    configure(fd,&o);
    if (o.scale) scalar(owner,"Research scaler test",o.scale);
    int heap=o.heap ? open("/dev/dma_heap/system",O_RDWR|O_CLOEXEC) : -1;
    if (o.heap && heap<0) die("system heap");
    struct image im[2][4]; struct buffer buf[2][4];
    FILE *input=fopen(argv[6],"rb"),*output=fopen(argv[7],"wb");
    if (!input || !output) die("raw file");
    for (unsigned q=0;q<2;q++) {
        unsigned qw=q && o.dw ? o.dw : w,qh=q && o.dh ? o.dh : h;
        struct image base=setfmt(fd,q,qw,qh,ls[q],&o);
        setfmt(owner,q,qw,qh,ls[q],&o);
        struct v4l2_requestbuffers r={ .type=type(q),.memory=V4L2_MEMORY_MMAP,.count=count[q] };
        if (!o.heap) {
            checked(owner,VIDIOC_REQBUFS,&r,"REQBUFS allocator");
            if (r.count<count[q]) return 1;
        }
        r.memory=V4L2_MEMORY_DMABUF; checked(fd,VIDIOC_REQBUFS,&r,"REQBUFS importer");
        for (unsigned n=0;n<count[q];n++) {
            struct v4l2_buffer b={ .type=type(q),.memory=V4L2_MEMORY_MMAP,.index=n };
            if (o.heap) {
                struct dma_heap_allocation_data request={
                    .len=(base.size+4095)&~4095ULL,.fd_flags=O_RDWR|O_CLOEXEC };
                checked(heap,DMA_HEAP_IOCTL_ALLOC,&request,"DMA_HEAP_IOCTL_ALLOC");
                buf[q][n]=(struct buffer){ .size=request.len,.dmafd=(int)request.fd };
                buf[q][n].p=mmap(NULL,request.len,PROT_READ|PROT_WRITE,MAP_SHARED,request.fd,0);
            } else {
                checked(owner,VIDIOC_QUERYBUF,&b,"QUERYBUF");
                buf[q][n]=(struct buffer){ .size=b.length,.offset=b.m.offset,.dmafd=-1 };
                buf[q][n].p=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,owner,b.m.offset);
                struct v4l2_exportbuffer e={ .type=type(q),.index=n,.flags=O_CLOEXEC|O_RDWR };
                checked(owner,VIDIOC_EXPBUF,&e,"EXPBUF");buf[q][n].dmafd=e.fd;
            }
            if (buf[q][n].p==MAP_FAILED) die("mmap");
            im[q][n]=base;im[q][n].p=buf[q][n].p;
            struct context c={ .dmabuf=true };c.buf[q]=buf[q][n];
            sync_buffer(&c,q,true);memset(buf[q][n].p,q ? 0xcc : 0xa5,buf[q][n].size);
            if (!q) raw(&im[q][n],ls[q],input,false);
            sync_buffer(&c,q,false);
            b=(struct v4l2_buffer){ .type=type(q),.memory=V4L2_MEMORY_DMABUF,.index=n,
                .m.fd=o.alias == 1 && q ? buf[0][0].dmafd :
                      o.alias == 2 && q && n ? buf[1][0].dmafd :
                      o.alias == 3 && q ? buf[0][1].dmafd : buf[q][n].dmafd,.length=buf[q][n].size,.bytesused=base.size,
                .timestamp={ .tv_sec=1,.tv_usec=n*40000 } };
            checked(fd,VIDIOC_QBUF,&b,"QBUF experiment");
        }
    }
    if (fgetc(input)!=EOF) { fputs("oversized input\n",stderr);return 1; } fclose(input);
    uint64_t timestamps[4]={0};
    uint64_t start=now_ns(),dma_finished=0;
    for (unsigned q=0;q<2;q++) { enum v4l2_buf_type t=type(q);checked(fd,VIDIOC_STREAMON,&t,"STREAMON"); }
    if (o.abort_job) goto stop;
    if (o.dei) {
        struct v4l2_control mode={ .id=control_by_name(fd,"Deinterlace mode"), .value=o.dei==1 ? 2 : 1 };
        if (!xioctl(fd,VIDIOC_S_CTRL,&mode) || errno!=EBUSY) {
            fputs("temporal mode changed during streaming\n",stderr);return 1;
        }
        checked(fd,VIDIOC_G_CTRL,&mode,"G_CTRL rejected temporal mode");
        if (mode.value!=(int)o.dei) return 1;
    }
    for (unsigned n=0;n<count[1];n++) {
        struct v4l2_buffer b={ .type=type(1),.memory=V4L2_MEMORY_DMABUF };
        uint64_t end=now_ns()+3000000000ULL;
        while (xioctl(fd,VIDIOC_DQBUF,&b)) {
            if (errno!=EAGAIN) die("DQBUF experiment");
            if (now_ns()>end) { fputs("DQBUF deadline\n",stderr);return 1; }
            struct pollfd pollfd={ .fd=fd,.events=POLLIN };int rc=poll(&pollfd,1,100);
            if (rc<0 && errno!=EINTR) die("poll");
        }
        dma_finished=now_ns();
        if (o.reject) {
            if (!(b.flags&V4L2_BUF_FLAG_ERROR)) { fputs("expected rejected job\n",stderr);return 1; }
            goto stop;
        }
        if (b.flags&V4L2_BUF_FLAG_ERROR || b.field!=V4L2_FIELD_NONE || b.sequence!=n || b.index>=count[1]) {
            fprintf(stderr,"bad capture flags=%x field=%u seq=%u expected=%u\n",b.flags,b.field,b.sequence,n);return 1;
        }
        timestamps[n]=(uint64_t)b.timestamp.tv_sec*1000000+b.timestamp.tv_usec;
        struct context c={ .dmabuf=true };c.buf[1]=buf[1][b.index];
        sync_buffer(&c,1,true);raw(&im[1][b.index],ls[1],output,true);sync_buffer(&c,1,false);
    }
    if (o.dei) for (unsigned n=0;n<count[1];n++) {
        uint64_t expected=1020000ULL+n*(o.dei==2 ? 20000ULL : 40000ULL);
        if (timestamps[n]!=expected) { fputs("incorrect field timestamp\n",stderr);return 1; }
    }
    /* The final temporal source is retained until STREAMOFF; earlier sources
     * must already be returned after their last hardware read. */
    unsigned returned=0;
    for (;;) {
        struct v4l2_buffer b={ .type=type(0),.memory=V4L2_MEMORY_DMABUF };
        if (xioctl(fd,VIDIOC_DQBUF,&b)) { if (errno!=EAGAIN) die("DQBUF source");break; }
        if (b.flags&V4L2_BUF_FLAG_ERROR || b.sequence!=returned++) return 1;
    }
    if (returned!=(o.dei ? count[0]-1 : count[0])) { fputs("history lifetime violated\n",stderr);return 1; }
stop:
    for (unsigned q=0;q<2;q++) { enum v4l2_buf_type t=type(q);checked(fd,VIDIOC_STREAMOFF,&t,"STREAMOFF"); }
    for (unsigned q=0;q<2;q++) for (unsigned n=0;n<count[q];n++) {
        if (munmap(buf[q][n].p,buf[q][n].size)) die("munmap");
        close(buf[q][n].dmafd);
    }
    fclose(output);
    if (heap>=0) close(heap);
    if (o.dei) scalar(fd,"Deinterlace mode",0);
    close(fd);close(owner);
    printf("{\"src\":\"%s\",\"dst\":\"%s\",\"outputs\":%u,\"hw_us\":%.3f,\"dma_us\":%.3f,\"ioctl_pass\":true,\"timestamps_us\":[%llu,%llu,%llu,%llu],\"strides\":[%u,%u]}\n",
           argv[4],argv[5],count[1],(now_ns()-start)/1000.0,(dma_finished ? dma_finished-start : 0)/1000.0,(unsigned long long)timestamps[0],
           (unsigned long long)timestamps[1],(unsigned long long)timestamps[2],(unsigned long long)timestamps[3],
           im[0][0].stride,im[1][0].stride);
    return 0;
}
