/* Author-default JPEG selection, fixed for the camera JOB worker.
 * Selection and 2x box reduction follow DevCrashDay/a6400 ritual_daemon.c,
 * de73e850044da4e5872be1ee93e4dc71684f2851 (SHA256 139ab443...54c22ce).
 * Keep this job's bounded I/O, existing codecs, exclusive output and metadata.
 * No SD mounts, original writes, UI options or runtime quality modes.
 */
#ifndef A6400_AUTHOR_FAST_JPEG_H
#define A6400_AUTHOR_FAST_JPEG_H

#include "film_preview_fmt.h"

#define AUTHOR_SCAN_BYTES 1500000u
#define AUTHOR_RGB_BYTES (24u*1024u*1024u)
#define AUTHOR_12M_TARGET_PIXELS 12000000ULL
#define AUTHOR_12M_PASSTHROUGH_MAX 12500000ULL

static unsigned film_clock_ms() {
#ifdef A6400_FILM_HOST
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (unsigned)((unsigned long long)value.tv_sec*1000u+
                      (unsigned long)value.tv_nsec/1000000u);
#else
    return now_ms();
#endif
}

/* Find the codestream end, never a byte-pattern match inside APP metadata. */
static unsigned author_jpeg_length(const unsigned char *data,unsigned size,unsigned start) {
    if (start>=size || size-start<5 || data[start]!=0xff || data[start+1]!=0xd8) return 0;
    unsigned at=start+2;
    int scan=0;
    while (at+1<size) {
        if (scan) {
            if (data[at]==0xff && data[at+1]==0xd9) return at+2-start;
            ++at;
        } else {
            if (data[at++]!=0xff) continue;
            while (at<size && data[at]==0xff) ++at;
            if (at>=size) break;
            unsigned marker=data[at++];
            if (marker==0xd9) return at-start;
            if (marker==0xd8 || marker==0 || marker==1 ||
                (marker>=0xd0 && marker<=0xd7)) continue;
            if (size-at<2) break;
            unsigned length=((unsigned)data[at]<<8)|data[at+1];
            if (length<2 || length>size-at) break;
            at+=length;
            if (marker==0xda) scan=1;
        }
    }
    return 0;
}

/* JPEG fast path in the author's code scans only the final 1,500,000 bytes
 * and selects the largest complete nested JPEG above 40,000 bytes. */
static int author_preview(int fd,unsigned *offset,unsigned *length) {
    *offset=0; *length=0;
    long size=sys_lseek(fd,0,2);
    if (size<0 || (unsigned long)size>MAX_INPUT_BYTES) return -1;
    if ((unsigned long)size<AUTHOR_SCAN_BYTES) return 0;
    unsigned char *tail=(unsigned char *)xalloc(AUTHOR_SCAN_BYTES);
    if (!tail) return -1;
    long start=size-(long)AUTHOR_SCAN_BYTES;
    int complete=sys_lseek(fd,start,0)==start &&
        jpeg_read_exact(&fd,source_read,tail,AUTHOR_SCAN_BYTES);
    unsigned best=0,best_length=0;
    if (complete) for (unsigned i=0;i+16<AUTHOR_SCAN_BYTES;++i) {
        if (tail[i]==0xff && tail[i+1]==0xd8 && tail[i+2]==0xff &&
            (tail[i+3]==0xe1 || tail[i+3]==0xe0 || tail[i+3]==0xdb)) {
            unsigned bytes=author_jpeg_length(tail,AUTHOR_SCAN_BYTES,i);
            if (bytes>40000u && bytes>best_length) {
                best=i; best_length=bytes; i+=bytes-1;
            }
        }
    }
    xfree(tail,AUTHOR_SCAN_BYTES);
    if (!complete) return -1;
    if (best_length) {*offset=(unsigned)start+best;*length=best_length;}
    return best_length?1:0;
}

struct author_input {
    int fd,failed;
    unsigned left,pos,end;
    unsigned char buf[16384];
};
static unsigned char author_read(unsigned char *buffer,unsigned char capacity,
                                 unsigned char *count,void *context) {
    author_input *input=(author_input *)context;
    if (input->pos==input->end) {
        unsigned chunk=input->left<(unsigned)sizeof(input->buf)?input->left:(unsigned)sizeof(input->buf);
        long amount=chunk?sys_read(input->fd,input->buf,chunk):0;
        if (amount<0 || (unsigned long)amount>chunk) {
            input->failed=1; *count=0; return PJPG_STREAM_READ_ERROR;
        }
        input->left-=(unsigned)amount;
        input->pos=0;
        input->end=(unsigned)amount;
    }
    unsigned avail=input->end-input->pos;
    unsigned n=avail<capacity?avail:capacity;
    if (n) {
        memcpy(buffer,input->buf+input->pos,n);
        input->pos+=n;
    }
    *count=(unsigned char)n;
    return 0;
}

struct author_image {
    unsigned char *pixels;
    unsigned bytes;
    int width,height,source_width,source_height,reduced;
    author_image():pixels(NULL),bytes(0),width(0),height(0),
        source_width(0),source_height(0),reduced(0) {}
    void clear() {if(pixels) xfree(pixels,bytes);pixels=NULL;bytes=0;}
    ~author_image() {clear();}
};

static inline int author_fast_memory_ok(const char *text,unsigned size,unsigned rgb_bytes,
                                        unsigned strip_bytes,unsigned mask_bytes,
                                        unsigned *available,unsigned *required) {
    const char *names[]={"MemFree:","Buffers:","Cached:","Shmem:"};
    unsigned values[4]={0,0,0,0},seen=0;
    *available=0;*required=0;
    if (rgb_bytes>AUTHOR_RGB_BYTES || strip_bytes>524288u || mask_bytes>2097152u) return 0;
    for (unsigned at=0;at<size;) {
        unsigned end=at;
        while (end<size && text[end]!='\n') ++end;
        if (end==size) return 0;
        for (unsigned key=0;key<4;++key) {
            unsigned n=(unsigned)strlen(names[key]);
            if (n>end-at || strncmp(text+at,names[key],n)) continue;
            if (seen&(1u<<key)) return 0;
            unsigned p=at+n,value=0,digits=0;
            while (p<end && (text[p]==' ' || text[p]=='\t')) ++p;
            while (p<end && text[p]>='0' && text[p]<='9') {
                if (++digits>9) return 0;
                value=value*10u+(unsigned)(text[p++]-'0');
            }
            if (!digits) return 0;
            while (p<end && (text[p]==' ' || text[p]=='\t')) ++p;
            if (end-p<2 || text[p]!='k' || text[p+1]!='B') return 0;
            p+=2;
            while (p<end && (text[p]==' ' || text[p]=='\t')) ++p;
            if (p!=end) return 0;
            values[key]=value;seen|=1u<<key;
        }
        at=end+1;
    }
    if (seen!=15u || values[0]+values[1]+values[2]<values[3]) return 0;
    *available=values[0]+values[1]+values[2]-values[3];
    unsigned out_reserve=(rgb_bytes>8u*1024u*1024u)?(6u*1024u*1024u):MAX_OUTPUT_BYTES;
    *required=(rgb_bytes+strip_bytes+mask_bytes+MAX_HEAP_BLOCK+
               out_reserve+8u*1024u*1024u+1023u)/1024u;
    return *available>=*required;
}

static int author_memory_ready(unsigned rgb_bytes,unsigned strip_bytes,unsigned mask_bytes) {
#ifdef A6400_FILM_HOST
    (void)rgb_bytes;(void)strip_bytes;(void)mask_bytes;
    return 1;
#else
    int fd=sys_open("/proc/meminfo",O_RDONLY|O_NOFOLLOW,0);
    if (fd<0) return 0;
    char buffer[4096];
    long count=sys_read(fd,buffer,sizeof(buffer));
    int closed=sys_close(fd);
    unsigned available=0,required=0;
    int ok=count>0 && (unsigned long)count<sizeof(buffer) && !closed &&
        author_fast_memory_ok(buffer,(unsigned)count,rgb_bytes,strip_bytes,mask_bytes,&available,&required);
    printf("FILM_MEMORY available_kib=%u required_kib=%u rgb_bytes=%u ok=%d\n",
        available,required,rgb_bytes,ok);
    return ok;
#endif
}

static int author_decode(int fd,unsigned offset,unsigned length,int fallback,author_image *image) {
    if (!length || sys_lseek(fd,(long)offset,0)!=(long)offset) return 4;
    author_input input; input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0; pjpeg_image_info_t info={};
    unsigned status=pjpeg_decode_init(&info,author_read,&input,0);
    if (status || input.failed || info.m_width<1 || info.m_height<1 ||
        (unsigned)info.m_width>MAX_IMAGE_DIMENSION ||
        (unsigned)info.m_height>MAX_IMAGE_DIMENSION ||
        (unsigned long)info.m_width*(unsigned long)info.m_height>MAX_PIXELS ||
        info.m_MCUHeight<1 || info.m_MCUHeight>16 ||
        (info.m_comps!=1 && info.m_comps!=3)) return 4;
    int scale=fallback && info.m_width>2400?2:1;
    if (scale==2) {
        if (sys_lseek(fd,(long)offset,0)!=(long)offset) return 4;
        input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
        memset(&info,0,sizeof(info));
        status=pjpeg_decode_init(&info,author_read,&input,2);
        if (status || input.failed || info.m_MCUWidth<2 || info.m_MCUHeight<2) return 4;
    }
    int width=info.m_width/scale,height=info.m_height/scale;
    if (width<1 || height<1 ||
        (unsigned long)width*(unsigned long)height>AUTHOR_RGB_BYTES/3u) return 5;
    image->bytes=(unsigned)width*(unsigned)height*3u;
    unsigned strip_bytes=(scale==2)?0u:(unsigned)info.m_width*3u*(unsigned)info.m_MCUHeight;
    unsigned mask_bytes=(unsigned)(width/4)*(unsigned)(height/4)*2u;
    if (!author_memory_ready(image->bytes,strip_bytes,mask_bytes)) return 5;
    image->pixels=(unsigned char *)xalloc(image->bytes);
    if (!image->pixels) return 5;
    if (scale==1) {
        workspace work;
        work.strip_size=(size_t)info.m_width*3u*(unsigned)info.m_MCUHeight;
        work.strip=(unsigned char *)xalloc(work.strip_size);
        if (!work.strip) {image->clear();return 5;}
        unsigned pitch=(unsigned)info.m_width*3u;
        for (int row=0;row<info.m_MCUSPerCol;++row) {
            if (fill_strip(&info,work.strip) || input.failed) {image->clear();return 8;}
            int first_y=row*info.m_MCUHeight;
            int rows=info.m_height-first_y<info.m_MCUHeight?info.m_height-first_y:info.m_MCUHeight;
            memcpy(image->pixels+(unsigned long)first_y*pitch,work.strip,(size_t)rows*pitch);
        }
    } else {
        int rmw=info.m_MCUWidth>>1, rmh=info.m_MCUHeight>>1;
        for (int row=0;row<info.m_MCUSPerCol;++row) {
            int dst_y=row*rmh;
            int rows=height-dst_y<rmh?height-dst_y:rmh;
            for (int mcu_x=0;mcu_x<info.m_MCUSPerRow;++mcu_x) {
                if (pjpeg_decode_mcu() || input.failed) {image->clear();return 8;}
                int dst_x=mcu_x*rmw;
                if (dst_x>=width || rows<=0) continue;
                int cols=width-dst_x<rmw?width-dst_x:rmw;
                for (int y=0;y<rows;++y) {
                    unsigned char *out=image->pixels+((unsigned long)(dst_y+y)*(unsigned)width+(unsigned)dst_x)*3u;
                    unsigned src_row=(unsigned)y*(unsigned)rmw;
                    if (info.m_comps==1) {
                        for (int x=0;x<cols;++x) {
                            unsigned char v=info.m_pMCUBufR[src_row+(unsigned)x];
                            out[x*3]=v; out[x*3+1]=v; out[x*3+2]=v;
                        }
                    } else {
                        for (int x=0;x<cols;++x) {
                            unsigned idx=src_row+(unsigned)x;
                            out[x*3]=info.m_pMCUBufR[idx];
                            out[x*3+1]=info.m_pMCUBufG[idx];
                            out[x*3+2]=info.m_pMCUBufB[idx];
                        }
                    }
                }
            }
        }
    }
    if (pjpeg_decode_mcu()!=PJPG_NO_MORE_BLOCKS || input.failed) {image->clear();return 8;}
    image->width=width;image->height=height;image->source_width=info.m_width;
    image->source_height=info.m_height;image->reduced=scale==2;
    return 0;
}

static unsigned author_u16(const unsigned char *p,int little) {
    return little?(unsigned)p[0]|((unsigned)p[1]<<8):((unsigned)p[0]<<8)|p[1];
}
static unsigned author_u32(const unsigned char *p,int little) {
    return little?author_u16(p,1)|(author_u16(p+2,1)<<16):
        (author_u16(p,0)<<16)|author_u16(p+2,0);
}
static void author_put(unsigned char *p,unsigned value,unsigned bytes,int little) {
    for (unsigned i=0;i<bytes;++i) p[little?i:bytes-1-i]=(unsigned char)(value>>(i*8u));
}
static unsigned author_ifd(unsigned char *tiff,unsigned size,unsigned at,int little,
                            unsigned width,unsigned height) {
    if (at<8 || at>size || size-at<2) return 0;
    unsigned count=author_u16(tiff+at,little),exif=0;
    if (count>(size-at-2)/12u) return 0;
    for (unsigned i=0;i<count;++i) {
        unsigned char *entry=tiff+at+2+i*12u;
        unsigned tag=author_u16(entry,little),type=author_u16(entry+2,little);
        if (author_u32(entry+4,little)!=1) continue;
        if (tag==34665 && type==4) exif=author_u32(entry+8,little);
        if ((tag==256 || tag==257 || tag==40962 || tag==40963) && (type==3 || type==4))
            author_put(entry+8,(tag==256 || tag==40962)?width:height,type==3?2u:4u,little);
    }
    return exif;
}
static void author_metadata_size(unsigned char *metadata,unsigned size,unsigned width,unsigned height) {
    for (unsigned at=0;at+4<=size;) {
        unsigned length=((unsigned)metadata[at+2]<<8)|metadata[at+3];
        if (length<2 || length>size-at-2) return;
        if (metadata[at]==0xff && metadata[at+1]==0xe1 && length>=16 &&
            metadata[at+4]=='E' && metadata[at+5]=='x' && metadata[at+6]=='i' &&
            metadata[at+7]=='f' && metadata[at+8]==0 && metadata[at+9]==0) {
            unsigned char *tiff=metadata+at+10;
            unsigned available=length-8;
            int little=tiff[0]=='I' && tiff[1]=='I';
            if ((little || (tiff[0]=='M' && tiff[1]=='M')) && author_u16(tiff+2,little)==42) {
                unsigned exif=author_ifd(tiff,available,author_u32(tiff+4,little),little,width,height);
                if (exif) author_ifd(tiff,available,exif,little,width,height);
            }
        }
        at+=length+2;
    }
}

static unsigned author_metadata_orientation(const unsigned char *metadata,unsigned size) {
    for (unsigned at=0;at+4<=size;) {
        unsigned length=((unsigned)metadata[at+2]<<8)|metadata[at+3];
        if (length<2 || length>size-at-2) return 1u;
        if (metadata[at]==0xff && metadata[at+1]==0xe1 && length>=16 &&
            metadata[at+4]=='E' && metadata[at+5]=='x' && metadata[at+6]=='i' &&
            metadata[at+7]=='f' && metadata[at+8]==0 && metadata[at+9]==0) {
            const unsigned char *tiff=metadata+at+10;
            unsigned available=length-8;
            int little=tiff[0]=='I' && tiff[1]=='I';
            if ((little || (tiff[0]=='M' && tiff[1]=='M')) && author_u16(tiff+2,little)==42) {
                unsigned ifd=author_u32(tiff+4,little);
                if (ifd>=8 && ifd<=available && available-ifd>=2) {
                    unsigned count=author_u16(tiff+ifd,little);
                    if (count<=(available-ifd-2)/12u) {
                        for (unsigned i=0;i<count;++i) {
                            const unsigned char *e=tiff+ifd+2+i*12u;
                            if (author_u16(e,little)==274u && author_u16(e+2,little)==3u &&
                                author_u32(e+4,little)==1u) {
                                unsigned v=author_u16(e+8,little);
                                return (v==3u || v==6u || v==8u)?v:1u;
                            }
                        }
                    }
                }
            }
        }
        at+=length+2;
    }
    return 1u;
}

static void author_pack_preview_box(const unsigned char *rgb,int img_w,int img_h,
                                    unsigned orient,uint16_t *dst,
                                    unsigned box_w,unsigned box_h,int border) {
    const uint16_t bg_px=0x112Fu;     /* dark charcoal #111122 opaque */
    const uint16_t gold_px=0xEA3Fu;   /* warm accent outer border */
    const uint16_t frame_px=0x223Fu;  /* inner hairline */
    for (unsigned i=0;i<box_w*box_h;++i) dst[i]=bg_px;
    if (border && box_w>=8 && box_h>=8) {
        for (unsigned x=0;x<box_w;++x) {
            dst[x]=gold_px;
            dst[(box_h-1u)*box_w+x]=gold_px;
            if (x>0 && x+1<box_w) {
                dst[box_w+x]=frame_px;
                dst[(box_h-2u)*box_w+x]=frame_px;
            }
        }
        for (unsigned y=1;y+1<box_h;++y) {
            dst[y*box_w]=gold_px;
            dst[y*box_w+box_w-1u]=gold_px;
            if (y>1 && y+2<box_h) {
                dst[y*box_w+1u]=frame_px;
                dst[y*box_w+box_w-2u]=frame_px;
            }
        }
    }
    if (!rgb || img_w<1 || img_h<1) return;
    unsigned pad=border?2u:0u;
    unsigned avail_w=box_w>pad*2u?box_w-pad*2u:box_w;
    unsigned avail_h=box_h>pad*2u?box_h-pad*2u:box_h;
    unsigned rot_w=(orient==6u || orient==8u)?(unsigned)img_h:(unsigned)img_w;
    unsigned rot_h=(orient==6u || orient==8u)?(unsigned)img_w:(unsigned)img_h;
    unsigned fit_w,fit_h;
    if (rot_w*avail_h >= rot_h*avail_w) {
        fit_w=avail_w;
        fit_h=(avail_w*rot_h+rot_w/2u)/rot_w;
    } else {
        fit_h=avail_h;
        fit_w=(avail_h*rot_w+rot_h/2u)/rot_h;
    }
    if (fit_w<1u) fit_w=1u;
    if (fit_h<1u) fit_h=1u;
    if (fit_w>avail_w) fit_w=avail_w;
    if (fit_h>avail_h) fit_h=avail_h;
    unsigned off_x=pad+(avail_w-fit_w)/2u;
    unsigned off_y=pad+(avail_h-fit_h)/2u;
    int do_avg=(rot_w>=fit_w*2u && rot_h>=fit_h*2u);
    int do_bilin=(!do_avg && rot_w>=256u && rot_h>=256u && img_w>=2 && img_h>=2);
    for (unsigned y=0;y<fit_h;++y) {
        uint16_t *out_row=dst+(off_y+y)*box_w+off_x;
        if (do_bilin) {
            unsigned fy256=((y*(rot_h-1u))<<8)/(fit_h>1u?(fit_h-1u):1u);
            unsigned ly0=fy256>>8, wy=fy256&255u, iwy=256u-wy;
            if (ly0+1u>=rot_h) { ly0=rot_h-2u; wy=256u; iwy=0u; }
            for (unsigned x=0;x<fit_w;++x) {
                unsigned fx256=((x*(rot_w-1u))<<8)/(fit_w>1u?(fit_w-1u):1u);
                unsigned lx0=fx256>>8, wx=fx256&255u, iwx=256u-wx;
                if (lx0+1u>=rot_w) { lx0=rot_w-2u; wx=256u; iwx=0u; }
                unsigned sx00,sy00,sx10,sy10,sx01,sy01,sx11,sy11;
                if (orient==6u) {
                    sx00=ly0; sy00=(unsigned)img_h-1u-lx0;
                    sx10=ly0; sy10=(unsigned)img_h-1u-(lx0+1u);
                    sx01=ly0+1u; sy01=sy00;
                    sx11=ly0+1u; sy11=sy10;
                } else if (orient==8u) {
                    sx00=(unsigned)img_w-1u-ly0; sy00=lx0;
                    sx10=sx00; sy10=lx0+1u;
                    sx01=(unsigned)img_w-1u-(ly0+1u); sy01=lx0;
                    sx11=sx01; sy11=lx0+1u;
                } else if (orient==3u) {
                    sx00=(unsigned)img_w-1u-lx0; sy00=(unsigned)img_h-1u-ly0;
                    sx10=(unsigned)img_w-1u-(lx0+1u); sy10=sy00;
                    sx01=sx00; sy01=(unsigned)img_h-1u-(ly0+1u);
                    sx11=sx10; sy11=sy01;
                } else {
                    sx00=lx0; sy00=ly0;
                    sx10=lx0+1u; sy10=ly0;
                    sx01=lx0; sy01=ly0+1u;
                    sx11=lx0+1u; sy11=ly0+1u;
                }
                const unsigned char *p00=rgb+((unsigned long)sy00*(unsigned)img_w+sx00)*3u;
                const unsigned char *p10=rgb+((unsigned long)sy10*(unsigned)img_w+sx10)*3u;
                const unsigned char *p01=rgb+((unsigned long)sy01*(unsigned)img_w+sx01)*3u;
                const unsigned char *p11=rgb+((unsigned long)sy11*(unsigned)img_w+sx11)*3u;
                unsigned r=((p00[0]*iwx+p10[0]*wx)*iwy+(p01[0]*iwx+p11[0]*wx)*wy+32768u)>>16;
                unsigned g=((p00[1]*iwx+p10[1]*wx)*iwy+(p01[1]*iwx+p11[1]*wx)*wy+32768u)>>16;
                unsigned b=((p00[2]*iwx+p10[2]*wx)*iwy+(p01[2]*iwx+p11[2]*wx)*wy+32768u)>>16;
                out_row[x]=a6400_rgb_to_rgba4444_dither(r,g,b,x,y);
            }
            continue;
        }
        unsigned ly=(y*rot_h)/fit_h;
        if (ly>=rot_h) ly=rot_h-1u;
        for (unsigned x=0;x<fit_w;++x) {
            unsigned lx=(x*rot_w)/fit_w;
            if (lx>=rot_w) lx=rot_w-1u;
            unsigned sx,sy;
            if (orient==6u) { sx=ly; sy=(unsigned)img_h-1u-lx; }
            else if (orient==8u) { sx=(unsigned)img_w-1u-ly; sy=lx; }
            else if (orient==3u) { sx=(unsigned)img_w-1u-lx; sy=(unsigned)img_h-1u-ly; }
            else { sx=lx; sy=ly; }
            if (sx>=(unsigned)img_w) sx=(unsigned)img_w-1u;
            if (sy>=(unsigned)img_h) sy=(unsigned)img_h-1u;
            unsigned r,g,b;
            if (do_avg && sx+1u<(unsigned)img_w && sy+1u<(unsigned)img_h) {
                const unsigned char *p0=rgb+((unsigned long)sy*(unsigned)img_w+sx)*3u;
                const unsigned char *p1=p0+(unsigned)img_w*3u;
                r=((unsigned)p0[0]+p0[3]+p1[0]+p1[3]+2u)>>2;
                g=((unsigned)p0[1]+p0[4]+p1[1]+p1[4]+2u)>>2;
                b=((unsigned)p0[2]+p0[5]+p1[2]+p1[5]+2u)>>2;
            } else {
                const unsigned char *p=rgb+((unsigned long)sy*(unsigned)img_w+sx)*3u;
                r=p[0]; g=p[1]; b=p[2];
            }
            out_row[x]=a6400_rgb_to_rgba4444_dither(r,g,b,x,y);
        }
    }
}

static int author_write_all(int fd,const void *buf,unsigned bytes) {
    const unsigned char *p=(const unsigned char *)buf;
    unsigned left=bytes;
    while (left) {
        long w=sys_write(fd,p,left);
        if (w<=0) return -1;
        p+=(unsigned)w;
        left-=(unsigned)w;
    }
    return 0;
}

/* Orientation lives at the start of Exif. A large APP1 thumbnail must not hide it. */
static unsigned author_file_orientation(const unsigned char *data,unsigned size) {
    unsigned at=(size>=2u && data[0]==0xff && data[1]==0xd8)?2u:0u;
    while(at+4u<=size && data[at]==0xff) {
        unsigned marker=data[at+1];
        if(marker==0xd9u || marker==0xdau) return 1u;
        unsigned length=((unsigned)data[at+2]<<8)|data[at+3];
        if(length<2u) return 1u;
        unsigned avail=size-(at+2u);
        if(marker==0xe1u && avail>=8u &&
           data[at+4]=='E' && data[at+5]=='x' && data[at+6]=='i' && data[at+7]=='f') {
            unsigned use=length<=avail?length:avail;
            if(use>1024u) use=1024u;
            unsigned char seg[2u+1024u];
            seg[0]=0xff; seg[1]=0xe1;
            for(unsigned i=0;i<use;i++) seg[2u+i]=data[at+2u+i];
            seg[2]=(unsigned char)(use>>8); seg[3]=(unsigned char)use;
            return author_metadata_orientation(seg,2u+use);
        }
        if(length>avail) return 1u;
        at+=length+2u;
    }
    return 1u;
}

#ifndef A6400_FILM_HOST
static int author_pv_out_allowed(const char *p) {
    if(!p) return 0;
    if(!strcmp(p,A6400_PV_CURRENT)) return 1;
    const char *pfx=A6400_PV_DIR "/c_";
    unsigned n=0;
    while(pfx[n]) { if(p[n]!=pfx[n]) return 0; ++n; }
    if(!p[n]) return 0;
    for(unsigned i=n;p[i];++i) {
        char c=p[i];
        if(c=='/' || (c=='.' && p[i+1]=='.')) return 0;
        if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='.'||c=='_')) return 0;
    }
    return 1;
}
#endif

static int author_decode_album_fast(int fd,unsigned length,author_image *image) {
    if(!length || sys_lseek(fd,0,0)!=0) return 4;
    author_input input; input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
    pjpeg_image_info_t info={};
    unsigned status=pjpeg_decode_init(&info,author_read,&input,0);
    if(status || input.failed || info.m_width<1 || info.m_height<1 ||
       (unsigned)info.m_width>MAX_IMAGE_DIMENSION ||
       (unsigned)info.m_height>MAX_IMAGE_DIMENSION ||
       (unsigned long)info.m_width*(unsigned long)info.m_height>MAX_PIXELS ||
       info.m_MCUHeight<1 || info.m_MCUHeight>16 ||
       (info.m_comps!=1 && info.m_comps!=3)) return 4;
    /* 12MP/24MP graded JPEGs (>= 3600x2400): use 1/8 DC-only decode (zero IDCT,
     * 4240x2826 -> 530x353 in ~25 ms vs ~900 ms). */
    if(info.m_width>=3600 && info.m_height>=2400 &&
       (info.m_MCUWidth>>3)>=1 && (info.m_MCUHeight>>3)>=1) {
        if(sys_lseek(fd,0,0)!=0) return 4;
        input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
        memset(&info,0,sizeof(info));
        if(pjpeg_decode_init(&info,author_read,&input,1) || input.failed) return 4;
        int bx_per_mcu=info.m_MCUWidth>>3, by_per_mcu=info.m_MCUHeight>>3;
        int width=(info.m_width+7)/8, height=(info.m_height+7)/8;
        if(width<1 || height<1) return 4;
        image->bytes=(unsigned)width*(unsigned)height*3u;
        if(!author_memory_ready(image->bytes,0u,0u)) return 5;
        image->pixels=(unsigned char *)xalloc(image->bytes);
        if(!image->pixels) return 5;
        for(int my=0;my<info.m_MCUSPerCol;++my) {
            for(int mx=0;mx<info.m_MCUSPerRow;++mx) {
                if(pjpeg_decode_mcu() || input.failed) { image->clear(); return 8; }
                for(int by=0;by<by_per_mcu;++by) {
                    int ry=my*by_per_mcu+by;
                    if(ry>=height) continue;
                    for(int bx=0;bx<bx_per_mcu;++bx) {
                        int rx=mx*bx_per_mcu+bx;
                        if(rx>=width) continue;
                        unsigned idx=(unsigned)by*128u+(unsigned)bx*64u;
                        unsigned char *out=image->pixels+((unsigned long)ry*(unsigned)width+(unsigned)rx)*3u;
                        if(info.m_comps==1) {
                            unsigned char v=info.m_pMCUBufR[idx];
                            out[0]=v; out[1]=v; out[2]=v;
                        } else {
                            out[0]=info.m_pMCUBufR[idx];
                            out[1]=info.m_pMCUBufG[idx];
                            out[2]=info.m_pMCUBufB[idx];
                        }
                    }
                }
            }
        }
        if(pjpeg_decode_mcu()!=PJPG_NO_MORE_BLOCKS || input.failed) { image->clear(); return 8; }
        image->width=width; image->height=height;
        image->source_width=info.m_width; image->source_height=info.m_height;
        image->reduced=1;
        return 0;
    }
    /* 1.7MP/6MP graded JPEGs (>= 1200x800): use 1/2-scale IDCT (808x540 / 1500x1000). */
    if(info.m_width>=1200 && info.m_height>=800 && info.m_MCUWidth>=2 && info.m_MCUHeight>=2) {
        if(sys_lseek(fd,0,0)!=0) return 4;
        input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
        memset(&info,0,sizeof(info));
        if(pjpeg_decode_init(&info,author_read,&input,2) || input.failed) return 4;
        int width=info.m_width/2, height=info.m_height/2;
        if(width<1 || height<1 || (unsigned long)width*(unsigned long)height>AUTHOR_RGB_BYTES/3u) return 5;
        image->bytes=(unsigned)width*(unsigned)height*3u;
        if(!author_memory_ready(image->bytes,0u,0u)) return 5;
        image->pixels=(unsigned char *)xalloc(image->bytes);
        if(!image->pixels) return 5;
        int rmw=info.m_MCUWidth>>1, rmh=info.m_MCUHeight>>1;
        for(int row=0;row<info.m_MCUSPerCol;++row) {
            int dst_y=row*rmh;
            int rows=height-dst_y<rmh?height-dst_y:rmh;
            for(int mcu_x=0;mcu_x<info.m_MCUSPerRow;++mcu_x) {
                if(pjpeg_decode_mcu() || input.failed) { image->clear(); return 8; }
                int dst_x=mcu_x*rmw;
                if(dst_x>=width || rows<=0) continue;
                int cols=width-dst_x<rmw?width-dst_x:rmw;
                for(int y=0;y<rows;++y) {
                    unsigned char *out=image->pixels+((unsigned long)(dst_y+y)*(unsigned)width+(unsigned)dst_x)*3u;
                    unsigned src_row=(unsigned)y*(unsigned)rmw;
                    if(info.m_comps==1) {
                        for(int x=0;x<cols;++x) {
                            unsigned char v=info.m_pMCUBufR[src_row+(unsigned)x];
                            out[x*3]=v; out[x*3+1]=v; out[x*3+2]=v;
                        }
                    } else {
                        for(int x=0;x<cols;++x) {
                            unsigned idx=src_row+(unsigned)x;
                            out[x*3]=info.m_pMCUBufR[idx];
                            out[x*3+1]=info.m_pMCUBufG[idx];
                            out[x*3+2]=info.m_pMCUBufB[idx];
                        }
                    }
                }
            }
        }
        if(pjpeg_decode_mcu()!=PJPG_NO_MORE_BLOCKS || input.failed) { image->clear(); return 8; }
        image->width=width; image->height=height;
        image->source_width=info.m_width; image->source_height=info.m_height;
        image->reduced=1;
        return 0;
    }
    return author_decode(fd,0,length,1,image);
}

/* One 600x400 album preview. Does not rerun the film grade. */
static int author_album_preview_path(const char *jpeg_path,const char *out_path,const char *label) {
#ifndef A6400_FILM_HOST
    if(!jpeg_path || strcmp(jpeg_path,A6400_PV_JOB_INPUT) ||
       !author_pv_out_allowed(out_path)) return 2;
#endif
    int fd=sys_open(jpeg_path,O_RDONLY|O_NOFOLLOW,0);
    if(fd<0) return 3;
    long size=sys_lseek(fd,0,2);
    if(size<=0 || (unsigned long)size>MAX_INPUT_BYTES || sys_lseek(fd,0,0)!=0) {
        sys_close(fd); return 3;
    }
    /* A partial copy still decodes as flat filler blocks; require the closing EOI. */
    unsigned char eoi[2]={0,0};
    if(size<4 || sys_lseek(fd,size-2,0)!=size-2 || sys_read(fd,eoi,2)!=2 ||
       eoi[0]!=0xff || eoi[1]!=0xd9) {
        sys_close(fd); return 7;
    }
    unsigned char *orient_buf=(unsigned char *)xalloc(8192u);
    unsigned orient=1u;
    if(orient_buf) {
        if(sys_lseek(fd,0,0)==0) {
            long n=sys_read(fd,orient_buf,8192u);
            if(n>4) orient=author_file_orientation(orient_buf,(unsigned)n);
        }
        xfree(orient_buf,8192u);
    }
    author_image image;
    int decoded=author_decode_album_fast(fd,(unsigned)size,&image);
    int closed=sys_close(fd);
    if(decoded || closed) {image.clear(); return decoded?decoded:3;}
    uint16_t *alb=(uint16_t *)xalloc(A6400_ALBUM_BYTES);
    if(!alb) {image.clear(); return 5;}
    author_pack_preview_box(image.pixels,image.width,image.height,orient,alb,A6400_ALBUM_W,A6400_ALBUM_H,0);
    struct a6400_preview_hdr hdr;
    memset(&hdr,0,sizeof(hdr));
    hdr.magic=A6400_PV_MAGIC;
    hdr.width=A6400_ALBUM_W;
    hdr.height=A6400_ALBUM_H;
    if(label) {
        unsigned n=0;
        while(label[n] && n+1u<sizeof(hdr.label)) {hdr.label[n]=label[n]; ++n;}
    }
    int pfd=sys_open(out_path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    int ok=pfd>=0 && !author_write_all(pfd,&hdr,(unsigned)sizeof(hdr)) &&
        !author_write_all(pfd,alb,A6400_ALBUM_BYTES);
    if(pfd>=0 && sys_close(pfd)) ok=0;
    if(!ok && pfd>=0) sys_unlink(out_path);
    xfree(alb,A6400_ALBUM_BYTES);
    image.clear();
    return ok?0:6;
}

static void author_emit_preview_pkg(const char *preview_path,const unsigned char *rgb,
                                    int img_w,int img_h,unsigned orient) {
    if (!preview_path || !preview_path[0] || !rgb || img_w<1 || img_h<1) return;
    unsigned pop_bytes=A6400_POPUP_BYTES;
    unsigned alb_bytes=A6400_ALBUM_BYTES;
    uint16_t *pop=(uint16_t *)xalloc(pop_bytes);
    uint16_t *alb=(uint16_t *)xalloc(alb_bytes);
    if (!pop || !alb) {
        if (pop) xfree(pop,pop_bytes);
        if (alb) xfree(alb,alb_bytes);
        printf("FILM_PREVIEW_SKIP reason=alloc\n");
        return;
    }
    author_pack_preview_box(rgb,img_w,img_h,orient,pop,A6400_POPUP_W,A6400_POPUP_H,1);
    author_pack_preview_box(rgb,img_w,img_h,orient,alb,A6400_ALBUM_W,A6400_ALBUM_H,0);
    struct a6400_preview_hdr h_pop,h_alb;
    memset(&h_pop,0,sizeof(h_pop));
    memset(&h_alb,0,sizeof(h_alb));
    h_pop.magic=A6400_PV_MAGIC; h_pop.width=A6400_POPUP_W; h_pop.height=A6400_POPUP_H;
    h_alb.magic=A6400_PV_MAGIC; h_alb.width=A6400_ALBUM_W; h_alb.height=A6400_ALBUM_H;
    int pfd=sys_open(preview_path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    int ok=0;
    if (pfd>=0) {
        ok=!author_write_all(pfd,&h_pop,(unsigned)sizeof(h_pop)) &&
           !author_write_all(pfd,pop,pop_bytes) &&
           !author_write_all(pfd,&h_alb,(unsigned)sizeof(h_alb)) &&
           !author_write_all(pfd,alb,alb_bytes);
        if (sys_close(pfd)) ok=0;
        if (!ok) sys_unlink(preview_path);
    }
    xfree(pop,pop_bytes);
    xfree(alb,alb_bytes);
    if (ok) printf("FILM_PREVIEW_OK bytes=%u orient=%u\n",(unsigned)A6400_PV_PKG_BYTES,orient);
    else printf("FILM_PREVIEW_SKIP reason=io\n");
}

static unsigned author_isqrt32(unsigned n) {
    if (!n) return 0u;
    unsigned x=1u<<30;
    while (x>n) x>>=2;
    unsigned res=0u;
    while (x) {
        if (n>=res+x) { n-=res+x; res=(res>>1)+x; }
        else res>>=1;
        x>>=2;
    }
    return res;
}

static void author_12m_dims(int src_w,int src_h,int *dst_w,int *dst_h) {
    unsigned pixels=(unsigned)src_w*(unsigned)src_h;
    if (pixels<=12500000u) {
        *dst_w=src_w; *dst_h=src_h;
        return;
    }
    unsigned w2_k=(12000u*(unsigned)src_w)/(unsigned)src_h;
    unsigned w=(author_isqrt32(w2_k*1000u)+8u)&~15u;
    if (w<64u) w=64u;
    if (w>(unsigned)src_w) w=(unsigned)src_w;
    unsigned h=((w*(unsigned)src_h)/(unsigned)src_w)&~1u;
    if (h<64u) h=64u;
    if (h>(unsigned)src_h) h=(unsigned)src_h;
    *dst_w=(int)w; *dst_h=(int)h;
}

/* Streaming strip-based 12MP renderer:
 *  - Optional Pass 1 uses picojpeg reduce=1 (1/8 DC-only) to build the 1/4-res halation mask
 *    in ~0.15s without decoding 24MP twice.
 *  - Single full-res Pass 2 decodes MCU row strips into a rolling (MCUHeight+1) ring buffer,
 *    applies pure-integer bilinear resize to ~12MP (4240x2826 for 6000x4000), runs film_pixels
 *    and film_halo_apply, and streams scanlines directly to jpge (peak RAM ~2.5 MiB).
 *  - Simultaneously samples a 600x400 RGB thumbnail for V4.0 instant popup & album preview.
 */
static int render_author_ultra12m(int fd,unsigned length,const char *path,
                                  int preset,int grain,int halo,
                                  unsigned char *metadata,unsigned metadata_size,
                                  const char *preview_path,unsigned orient) {
    unsigned begin=film_clock_ms();
    if (!length || sys_lseek(fd,0,0)!=0) return 4;
    author_input input; input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
    pjpeg_image_info_t info={};
    unsigned status=pjpeg_decode_init(&info,author_read,&input,0);
    if (status || input.failed || info.m_width<64 || info.m_height<64 ||
        (unsigned)info.m_width>MAX_IMAGE_DIMENSION ||
        (unsigned)info.m_height>MAX_IMAGE_DIMENSION ||
        (unsigned long)info.m_width*(unsigned long)info.m_height>MAX_PIXELS ||
        info.m_MCUHeight<1 || info.m_MCUHeight>16 ||
        (info.m_comps!=1 && info.m_comps!=3)) return 4;

    int src_w=info.m_width, src_h=info.m_height, mcu_h=info.m_MCUHeight;
    int dst_w=0, dst_h=0;
    author_12m_dims(src_w,src_h,&dst_w,&dst_h);
    int use_halo=(halo && preset!=4 && preset!=8 && (dst_w>>2)>=8 && (dst_h>>2)>=8);
    int gw=dst_w>>2, gh=dst_h>>2;
    unsigned mask_size=use_halo?(unsigned)gw*(unsigned)gh:0u;
    unsigned src_pitch=(unsigned)src_w*3u;
    unsigned strip_bytes=src_pitch*(unsigned)(mcu_h+1);
    unsigned out_row_bytes=(unsigned)dst_w*3u;
    unsigned pv_w=A6400_ALBUM_W, pv_h=A6400_ALBUM_H;
    unsigned pv_bytes=(preview_path && preview_path[0])?(pv_w*pv_h*3u):0u;
    unsigned table_bytes=(unsigned)dst_w*(unsigned)(sizeof(uint16_t)+sizeof(uint16_t));
    unsigned working_rgb=pv_bytes+out_row_bytes+table_bytes;
    if (!author_memory_ready(working_rgb,strip_bytes,mask_size*2u)) return 5;

    unsigned char *mask=NULL;
    if (use_halo) {
        if (sys_lseek(fd,0,0)!=0) return 4;
        author_input rinput; rinput.fd=fd; rinput.failed=0; rinput.left=length; rinput.pos=0; rinput.end=0;
        pjpeg_image_info_t rinfo={};
        if (pjpeg_decode_init(&rinfo,author_read,&rinput,1) || rinput.failed) return 4;
        int bx_per_mcu=rinfo.m_MCUWidth>>3;
        int by_per_mcu=rinfo.m_MCUHeight>>3;
        int red_w=rinfo.m_MCUSPerRow*bx_per_mcu;
        int red_h=rinfo.m_MCUSPerCol*by_per_mcu;
        if (red_w<1 || red_h<1) return 4;
        unsigned red_bytes=(unsigned)red_w*(unsigned)red_h;
        unsigned char *red_lum=(unsigned char *)xalloc(red_bytes);
        if (!red_lum) return 5;
        for (int my=0;my<rinfo.m_MCUSPerCol;++my) {
            for (int mx=0;mx<rinfo.m_MCUSPerRow;++mx) {
                if (pjpeg_decode_mcu() || rinput.failed) {xfree(red_lum,red_bytes);return 8;}
                for (int by=0;by<by_per_mcu;++by) {
                    int ry=my*by_per_mcu+by;
                    for (int bx=0;bx<bx_per_mcu;++bx) {
                        int rx=mx*bx_per_mcu+bx;
                        unsigned idx=(unsigned)by*128u+(unsigned)bx*64u;
                        unsigned lum;
                        if (rinfo.m_comps==1) lum=rinfo.m_pMCUBufR[idx];
                        else lum=(77u*rinfo.m_pMCUBufR[idx]+150u*rinfo.m_pMCUBufG[idx]+29u*rinfo.m_pMCUBufB[idx])>>8;
                        red_lum[(unsigned)ry*(unsigned)red_w+(unsigned)rx]=
                            (unsigned char)(lum>195u?(lum-195u)*4u:0u);
                    }
                }
            }
        }
        mask=(unsigned char *)xalloc(mask_size);
        unsigned char *scratch=(unsigned char *)xalloc(mask_size);
        if (!mask || !scratch) {
            if (mask) xfree(mask,mask_size);
            if (scratch) xfree(scratch,mask_size);
            xfree(red_lum,red_bytes);
            return 5;
        }
        for (int gy=0;gy<gh;++gy) {
            int ry=(int)(((unsigned)gy*(unsigned)red_h)/(unsigned)gh);
            if (ry>=red_h) ry=red_h-1;
            const unsigned char *rrow=red_lum+(unsigned)ry*(unsigned)red_w;
            unsigned char *mrow=mask+(unsigned)gy*(unsigned)gw;
            for (int gx=0;gx<gw;++gx) {
                int rx=(int)(((unsigned)gx*(unsigned)red_w)/(unsigned)gw);
                if (rx>=red_w) rx=red_w-1;
                mrow[gx]=rrow[rx];
            }
        }
        xfree(red_lum,red_bytes);
        film_mask_blur(mask,scratch,gw,gh);
        xfree(scratch,mask_size);
        /* Re-init full-resolution Pass 2 decoder. */
        if (sys_lseek(fd,0,0)!=0) {xfree(mask,mask_size);return 4;}
        input.fd=fd; input.failed=0; input.left=length; input.pos=0; input.end=0;
        memset(&info,0,sizeof(info));
        if (pjpeg_decode_init(&info,author_read,&input,0) || input.failed ||
            info.m_width!=src_w || info.m_height!=src_h) {
            xfree(mask,mask_size);
            return 4;
        }
    }
    unsigned mask_done=film_clock_ms();

    unsigned char *ring=(unsigned char *)xalloc(strip_bytes);
    unsigned char *out_row=(unsigned char *)xalloc(out_row_bytes);
    uint16_t *hx0=(uint16_t *)xalloc((unsigned)dst_w*sizeof(uint16_t));
    uint16_t *hfx=(uint16_t *)xalloc((unsigned)dst_w*sizeof(uint16_t));
    unsigned char *pv_rgb=pv_bytes?(unsigned char *)xalloc(pv_bytes):NULL;
    if (!ring || !out_row || !hx0 || !hfx || (pv_bytes && !pv_rgb)) {
        if (mask) xfree(mask,mask_size);
        if (ring) xfree(ring,strip_bytes);
        if (out_row) xfree(out_row,out_row_bytes);
        if (hx0) xfree(hx0,(unsigned)dst_w*sizeof(uint16_t));
        if (hfx) xfree(hfx,(unsigned)dst_w*sizeof(uint16_t));
        if (pv_rgb) xfree(pv_rgb,pv_bytes);
        return 5;
    }

    int same_size=(dst_w==src_w && dst_h==src_h);
    if (!same_size) {
        unsigned denom_x=(unsigned)(dst_w-1);
        for (int dx=0;dx<dst_w;++dx) {
            unsigned num=(unsigned)dx*(unsigned)(src_w-1);
            unsigned x0=num/denom_x;
            unsigned rem=num-x0*denom_x;
            unsigned fx=(rem*256u)/denom_x;
            if (x0+1u>=(unsigned)src_w) { x0=(unsigned)(src_w-2); fx=256u; }
            hx0[dx]=(uint16_t)x0;
            hfx[dx]=(uint16_t)fx;
        }
    }

    author_metadata_size(metadata,metadata_size,(unsigned)dst_w,(unsigned)dst_h);
    int out_fd=sys_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if (out_fd<0) {
        if (mask) xfree(mask,mask_size);
        xfree(ring,strip_bytes); xfree(out_row,out_row_bytes);
        xfree(hx0,(unsigned)dst_w*sizeof(uint16_t));
        xfree(hfx,(unsigned)dst_w*sizeof(uint16_t));
        if (pv_rgb) xfree(pv_rgb,pv_bytes);
        printf("FILM_ERROR output_open=%d\n",out_fd);
        return 6;
    }
    output_stream output(out_fd,metadata,metadata_size);
    jpge::params settings; settings.m_quality=88; settings.m_subsampling=jpge::H2V2;
    jpge::jpeg_encoder encoder;
    int result=encoder.init(&output,dst_w,dst_h,3,settings)?0:7;
    film_state film; film_init(&film,preset,grain);

    /* Rolling buffer: slot 0 holds last row of previous MCU strip (y = strip_y0 - 1),
     * slots 1..mcu_h hold current MCU strip rows (y = strip_y0 .. strip_y0 + mcu_h - 1). */
    int strip_y0=-mcu_h;
    int mcu_row=0;
    unsigned next_pv_y=0;
    for (int dy=0;!result && dy<dst_h;++dy) {
        int sy0,sy1;
        unsigned fy=0u;
        if (same_size) {
            sy0=sy1=dy;
        } else {
            unsigned denom_y=(unsigned)(dst_h-1);
            unsigned num=(unsigned)dy*(unsigned)(src_h-1);
            unsigned u0=num/denom_y;
            unsigned rem=num-u0*denom_y;
            sy0=(int)u0;
            fy=(rem*256u)/denom_y;
            if (sy0+1>=src_h) { sy0=src_h-2; fy=256u; }
            sy1=sy0+1;
        }
        while (sy1>=strip_y0+mcu_h) {
            if (mcu_row>=info.m_MCUSPerCol) { result=8; break; }
            if (mcu_row>0) memcpy(ring,ring+(size_t)mcu_h*src_pitch,src_pitch);
            if (fill_strip(&info,ring+src_pitch) || input.failed) { result=8; break; }
            strip_y0=mcu_row*mcu_h;
            if (mcu_row==0) memcpy(ring,ring+src_pitch,src_pitch);
            ++mcu_row;
        }
        if (result) break;
        const unsigned char *r0=ring+(size_t)(sy0-strip_y0+1)*src_pitch;
        const unsigned char *r1=ring+(size_t)(sy1-strip_y0+1)*src_pitch;
        if (same_size) {
            memcpy(out_row,r0,out_row_bytes);
        } else {
            unsigned ify=256u-fy;
            unsigned char *op=out_row;
            for (int dx=0;dx<dst_w;++dx,op+=3) {
                unsigned x0=hx0[dx];
                unsigned fx=hfx[dx];
                unsigned ifx=256u-fx;
                const unsigned char *p0=r0+x0*3u;
                const unsigned char *p1=r1+x0*3u;
                unsigned t0=p0[0]*ifx+p0[3]*fx;
                unsigned t1=p1[0]*ifx+p1[3]*fx;
                op[0]=(unsigned char)((t0*ify+t1*fy+32768u)>>16);
                t0=p0[1]*ifx+p0[4]*fx;
                t1=p1[1]*ifx+p1[4]*fx;
                op[1]=(unsigned char)((t0*ify+t1*fy+32768u)>>16);
                t0=p0[2]*ifx+p0[5]*fx;
                t1=p1[2]*ifx+p1[5]*fx;
                op[2]=(unsigned char)((t0*ify+t1*fy+32768u)>>16);
            }
        }
        film_pixels(&film,out_row,(unsigned long)dst_w);
        if (mask) film_halo_apply(out_row,dst_w,1,dy,mask,gw,gh,preset);
        if (!encoder.process_scanline(out_row)) { result=10; break; }
        if (pv_rgb) {
            while (next_pv_y<pv_h) {
                int want_y=(int)((next_pv_y*(unsigned)dst_h)/pv_h);
                if (want_y>dy) break;
                unsigned char *pvd=pv_rgb+(size_t)next_pv_y*pv_w*3u;
                for (unsigned px=0;px<pv_w;++px) {
                    unsigned sx=(px*(unsigned)dst_w)/pv_w;
                    const unsigned char *sp=out_row+sx*3u;
                    pvd[px*3u]=sp[0]; pvd[px*3u+1u]=sp[1]; pvd[px*3u+2u]=sp[2];
                }
                ++next_pv_y;
            }
        }
    }
    if (!result && !encoder.process_scanline(NULL)) result=11;
    encoder.deinit();
    int flushed=film_fsync(out_fd), closed=sys_close(out_fd);
    if (mask) xfree(mask,mask_size);
    xfree(ring,strip_bytes); xfree(out_row,out_row_bytes);
    xfree(hx0,(unsigned)dst_w*sizeof(uint16_t));
    xfree(hfx,(unsigned)dst_w*sizeof(uint16_t));
    if (result || input.failed || output.failed || flushed || closed || !output.bytes) {
        if (pv_rgb) xfree(pv_rgb,pv_bytes);
        sys_unlink(path);
        printf("FILM_ERROR ultra12m_render=%d output_failed=%d\n",result,output.failed);
        return result?result:12;
    }
    if (pv_rgb) {
        author_emit_preview_pkg(preview_path,pv_rgb,(int)pv_w,(int)pv_h,orient);
        xfree(pv_rgb,pv_bytes);
    }
    unsigned finished=film_clock_ms();
    printf("FILM_SOURCE kind=ultra12m source_width=%d source_height=%d width=%d height=%d offset=0 bytes=%u decode_passes=%d decode_attempts=1\n",
        src_w,src_h,dst_w,dst_h,length,use_halo?2:1);
    printf("FILM_TIMING select_ms=0 decode_ms=%u film_ms=0 encode_ms=%u total_ms=%u\n",
        mask_done-begin,finished-mask_done,finished-begin);
    printf("FILM_DONE width=%d height=%d bytes=%u rgb_kib=%u mask_kib=%u preset=%d grain=%d halation=%d\n",
        dst_w,dst_h,output.bytes,(strip_bytes+out_row_bytes+1023u)/1024u,
        (mask_size+1023u)/1024u,preset,grain,halo);
    return 0;
}

static int render_author_fast(int fd,const char *path,int preset,int grain,int halo,
                              unsigned char *metadata,unsigned metadata_size,int hires=0,
                              const char *preview_path=NULL) {
    unsigned orient=author_metadata_orientation(metadata,metadata_size);
    if (hires==2) {
        long size=sys_lseek(fd,0,2);
        if (size>=4 && (unsigned long)size<=MAX_INPUT_BYTES) {
            int r12=render_author_ultra12m(fd,(unsigned)size,path,preset,grain,halo,
                                           metadata,metadata_size,preview_path,orient);
            if (!r12) return 0;
            printf("FILM_ULTRA12M_FALLBACK result=%d\n",r12);
        }
        hires=1;
    }
    unsigned begin=film_clock_ms(),offset=0,length=0;
    int found=0;
    unsigned selected=0,attempts=1;
    author_image image;
    int result=4;
    if (hires) {
        long size=sys_lseek(fd,0,2);
        if (size>=4 && (unsigned long)size<=MAX_INPUT_BYTES) {
            selected=film_clock_ms();
            result=author_decode(fd,0,(unsigned)size,1,&image);
            length=(unsigned)size;
        }
        if (result) {
            printf("FILM_HIRES_FALLBACK result=%d\n",result);
            ++attempts;
            found=author_preview(fd,&offset,&length);
            if (found<0) return 4;
            selected=film_clock_ms();
            result=found?author_decode(fd,offset,length,0,&image):4;
        }
    } else {
        found=author_preview(fd,&offset,&length);
        if (found<0) return 4;
        selected=film_clock_ms();
        result=found?author_decode(fd,offset,length,0,&image):4;
        if (result) {
            if (found) ++attempts;
            long size=sys_lseek(fd,0,2);
            if (size<4 || (unsigned long)size>MAX_INPUT_BYTES) return 4;
            result=author_decode(fd,0,(unsigned)size,1,&image);
            found=0;offset=0;length=(unsigned)size;
        }
    }
    if (result) {printf("FILM_ERROR author_decode=%d\n",result);return result;}
    unsigned decoded=film_clock_ms();
    printf("FILM_SOURCE kind=%s source_width=%d source_height=%d width=%d height=%d offset=%u bytes=%u decode_passes=1 decode_attempts=%u\n",
        found?"embedded":image.reduced?"downsample2":"primary",image.source_width,image.source_height,
        image.width,image.height,offset,length,attempts);
    film_state film;film_init(&film,preset,grain);
    film_pixels(&film,image.pixels,(unsigned long)image.width*(unsigned)image.height);
    workspace work;
    int gw=image.width/4,gh=image.height/4;
    if (halo && preset!=4 && preset!=8 && gw>=8 && gh>=8) {
        work.mask_size=(size_t)gw*(unsigned)gh;
        work.mask=(unsigned char *)xalloc(work.mask_size);
        work.scratch=(unsigned char *)xalloc(work.mask_size);
        if (!work.mask || !work.scratch) return 5;
        film_mask_sample(work.mask,image.width,image.height,0,image.pixels,image.height);
        film_mask_blur(work.mask,work.scratch,gw,gh);
        xfree(work.scratch,work.mask_size);work.scratch=NULL;
        film_halo_apply(image.pixels,image.width,image.height,0,work.mask,gw,gh,preset);
        xfree(work.mask,work.mask_size);work.mask=NULL;
    }
    unsigned filtered=film_clock_ms();
    author_metadata_size(metadata,metadata_size,(unsigned)image.width,(unsigned)image.height);
    int out=sys_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if (out<0) {printf("FILM_ERROR output_open=%d\n",out);return 6;}
    output_stream output(out,metadata,metadata_size);
    jpge::params settings;settings.m_quality=90;settings.m_subsampling=jpge::H2V2;
    jpge::jpeg_encoder encoder;
    result=encoder.init(&output,image.width,image.height,3,settings)?0:7;
    for (int y=0;!result && y<image.height;++y)
        if (!encoder.process_scanline(image.pixels+(unsigned long)y*(unsigned)image.width*3u)) result=10;
    if (!result && !encoder.process_scanline(NULL)) result=11;
    encoder.deinit();
    int flushed=film_fsync(out),closed=sys_close(out);
    if (result || output.failed || flushed || closed || !output.bytes) {
        sys_unlink(path);printf("FILM_ERROR render=%d output_failed=%d\n",result,output.failed);return 12;
    }
    if (preview_path && preview_path[0]) {
        author_emit_preview_pkg(preview_path,image.pixels,image.width,image.height,orient);
    }
    unsigned finished=film_clock_ms();
    printf("FILM_TIMING select_ms=%u decode_ms=%u film_ms=%u encode_ms=%u total_ms=%u\n",
        selected-begin,decoded-selected,filtered-decoded,finished-filtered,finished-begin);
    printf("FILM_DONE width=%d height=%d bytes=%u rgb_kib=%u mask_kib=%u preset=%d grain=%d halation=%d\n",
        image.width,image.height,output.bytes,(image.bytes+1023u)/1024u,
        (unsigned)((work.mask_size+1023)/1024),preset,grain,halo);
    return 0;
}

static inline unsigned char author_blend_byte(unsigned a,unsigned b,int mode) {
    if (mode==(int)A6400_DEXP_LIGHTEN) return (unsigned char)(a>b?a:b);
    if (mode==(int)A6400_DEXP_AVERAGE) return (unsigned char)((a+b+1u)>>1);
    /* Default A6400_DEXP_SCREEN: 255 - round((255-a)*(255-b)/255) without division. */
    unsigned t=(255u-a)*(255u-b)+128u;
    return (unsigned char)(255u-((t+(t>>8))>>8));
}

static void author_blend_pixels(unsigned char *dst,int w1,int h1,
                                const unsigned char *src,int w2,int h2,int mode) {
    if (w1==w2 && h1==h2) {
        unsigned long total=(unsigned long)w1*(unsigned long)h1*3ul;
        for (unsigned long i=0;i<total;++i)
            dst[i]=author_blend_byte(dst[i],src[i],mode);
        return;
    }
    for (int y=0;y<h1;++y) {
        int sy=(int)(((unsigned long)y*(unsigned)h2)/(unsigned)h1);
        if (sy>=h2) sy=h2-1;
        unsigned char *drow=dst+(unsigned long)y*(unsigned)w1*3ul;
        const unsigned char *srow=src+(unsigned long)sy*(unsigned)w2*3ul;
        for (int x=0;x<w1;++x) {
            int sx=(int)(((unsigned long)x*(unsigned)w2)/(unsigned)w1);
            if (sx>=w2) sx=w2-1;
            unsigned char *dp=drow+(unsigned)x*3u;
            const unsigned char *sp=srow+(unsigned)sx*3u;
            dp[0]=author_blend_byte(dp[0],sp[0],mode);
            dp[1]=author_blend_byte(dp[1],sp[1],mode);
            dp[2]=author_blend_byte(dp[2],sp[2],mode);
        }
    }
}

static int author_select_and_decode(int fd,int want_hires,author_image *image) {
    unsigned offset=0,length=0;
    int found=0,result=4;
    if (want_hires) {
        long size=sys_lseek(fd,0,2);
        if (size>=4 && (unsigned long)size<=MAX_INPUT_BYTES)
            result=author_decode(fd,0,(unsigned)size,1,image);
        if (result) {
            found=author_preview(fd,&offset,&length);
            if (found>0) result=author_decode(fd,offset,length,0,image);
        }
    } else {
        found=author_preview(fd,&offset,&length);
        if (found>0) result=author_decode(fd,offset,length,0,image);
        if (result) {
            long size=sys_lseek(fd,0,2);
            if (size>=4 && (unsigned long)size<=MAX_INPUT_BYTES)
                result=author_decode(fd,0,(unsigned)size,1,image);
        }
    }
    return result;
}

/* Pure Double Exposure blender (no LUT applied, keeping Cortex-A5 CPU load minimal). */
static int render_author_dexp(int fd1,int fd2,const char *path,int blend_mode,
                              unsigned char *metadata,unsigned metadata_size,int hires,
                              const char *preview_path) {
    unsigned begin=film_clock_ms();
    unsigned orient=author_metadata_orientation(metadata,metadata_size);
    int want_hires=(hires>=1)?1:0;
    author_image img1;
    int r1=author_select_and_decode(fd1,want_hires,&img1);
    if (r1) { printf("FILM_ERROR dexp_decode1=%d\n",r1); return r1; }
    {
        author_image img2;
        int r2=author_select_and_decode(fd2,img1.reduced?1:0,&img2);
        if (r2 && img1.reduced) r2=author_select_and_decode(fd2,0,&img2);
        if (r2) { printf("FILM_ERROR dexp_decode2=%d\n",r2); return r2; }
        author_blend_pixels(img1.pixels,img1.width,img1.height,
                            img2.pixels,img2.width,img2.height,blend_mode);
    }
    unsigned blended=film_clock_ms();
    author_metadata_size(metadata,metadata_size,(unsigned)img1.width,(unsigned)img1.height);
    int out=sys_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if (out<0) { printf("FILM_ERROR output_open=%d\n",out); return 6; }
    output_stream output(out,metadata,metadata_size);
    jpge::params settings; settings.m_quality=90; settings.m_subsampling=jpge::H2V2;
    jpge::jpeg_encoder encoder;
    int result=encoder.init(&output,img1.width,img1.height,3,settings)?0:7;
    for (int y=0;!result && y<img1.height;++y)
        if (!encoder.process_scanline(img1.pixels+(unsigned long)y*(unsigned)img1.width*3u)) result=10;
    if (!result && !encoder.process_scanline(NULL)) result=11;
    encoder.deinit();
    int flushed=film_fsync(out),closed=sys_close(out);
    if (result || output.failed || flushed || closed || !output.bytes) {
        sys_unlink(path);
        printf("FILM_ERROR dexp_render=%d output_failed=%d\n",result,output.failed);
        return 12;
    }
    if (preview_path && preview_path[0])
        author_emit_preview_pkg(preview_path,img1.pixels,img1.width,img1.height,orient);
    unsigned finished=film_clock_ms();
    printf("FILM_TIMING select_ms=0 decode_ms=%u film_ms=0 encode_ms=%u total_ms=%u\n",
        blended-begin,finished-blended,finished-begin);
    printf("FILM_DONE width=%d height=%d bytes=%u rgb_kib=%u mask_kib=0 preset=%d grain=0 halation=0 dexp=%d\n",
        img1.width,img1.height,output.bytes,(img1.bytes+1023u)/1024u,blend_mode,blend_mode);
    return 0;
}
#endif
