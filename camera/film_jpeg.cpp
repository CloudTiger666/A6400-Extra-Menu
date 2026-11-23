/* Bounded two-pass author film rendering. No Sony library calls or input writes.
 * Color/grain/halation are compared pixel-for-pixel against the author's source.
 */
#ifdef A6400_FILM_HOST
#include "host/film_rt.h"
#else
#include "rt.h"
#define O_EXCL 0200
#define O_NOFOLLOW 0100000
static int film_fsync(int fd) {return (int)sys_call(118,fd,0,0,0,0,0);}
#endif
#include "jpeg_frame.h"
#include "author_film.h"
#ifndef A6400_FILM_HOST
#include "film_paths.h"
#endif
#include "../vendor/picojpeg.h"
#include "../vendor/jpge.h"
#define MAX_PIXELS 30000000UL
#define MAX_IMAGE_DIMENSION 8192u
#ifdef A6400_FILM_JOB
/* The private job tmpfs has its own total cap. ENOSPC still fails rendering
 * and removes only the exclusively created output; it never changes input. */
#define MAX_INPUT_BYTES (20u*1024u*1024u)
#define MAX_OUTPUT_BYTES (16u*1024u*1024u)
#else
#define MAX_INPUT_BYTES (64u*1024u*1024u)
#define MAX_OUTPUT_BYTES (64u*1024u*1024u)
#endif
#define MAX_HEAP_BLOCK (2u*1024u*1024u)
#ifndef A6400_FILM_HOST
/* jpge uses one malloc/free pair. Give it a bounded mmap-backed allocation. */
struct heap_block { size_t bytes, reserved; };
extern "C" void *malloc(size_t bytes) {
    if (!bytes || bytes > MAX_HEAP_BLOCK - sizeof(heap_block)) return NULL;
    heap_block *block = (heap_block *)xalloc(bytes + sizeof(heap_block));
    if (!block) return NULL;
    block->bytes = bytes + sizeof(heap_block);
    return block + 1;
}
extern "C" void free(void *pointer) {
    if (!pointer) return;
    heap_block *block = (heap_block *)pointer - 1;
    xfree(block, block->bytes);
}
void operator delete(void *pointer) noexcept { free(pointer); }
void operator delete[](void *pointer) noexcept { free(pointer); }
extern "C" void __cxa_pure_virtual(void) { sys_exit(93); }


#endif
struct input_state { int fd; int failed; };
static unsigned char read_jpeg(unsigned char *buffer, unsigned char capacity,
                               unsigned char *count, void *context) {
    input_state *input = (input_state *)context;
    long amount = sys_read(input->fd, buffer, capacity);
    if (amount < 0) {
        input->failed = 1;
        *count = 0;
        return PJPG_STREAM_READ_ERROR;
    }
    *count = (unsigned char)amount;
    return 0;
}

struct output_stream : jpge::output_stream {
    int fd;
    unsigned bytes;
    int failed;
    const unsigned char *metadata;
    unsigned metadata_size;
    unsigned char prefix[2];
    unsigned prefix_size;
    int started;
    output_stream(int descriptor, const unsigned char *app, unsigned app_size)
        : fd(descriptor), bytes(0), failed(0), metadata(app), metadata_size(app_size),
          prefix_size(0), started(0) {}
    bool write_bytes(const void *data, unsigned length) {
        if (length > MAX_OUTPUT_BYTES - bytes) { failed = 1; return false; }
        const unsigned char *cursor = (const unsigned char *)data;
        unsigned left = length;
        while (left) {
            long amount = sys_write(fd, cursor, left);
            if (amount <= 0) { failed = 1; return false; }
            cursor += amount;
            left -= (unsigned)amount;
            bytes += (unsigned)amount;
        }
        return true;
    }
    bool put_buf(const void *data, int length) override {
        const unsigned char *cursor = (const unsigned char *)data;
        if (length < 0) { failed = 1; return false; }
        while (prefix_size < 2 && length) {
            prefix[prefix_size++] = *cursor++;
            --length;
        }
        if (prefix_size == 2 && !started) {
            if (prefix[0] != 0xff || prefix[1] != 0xd8 ||
                !write_bytes(prefix, 2) || !write_bytes(metadata, metadata_size))
                return false;
            started = 1;
        }
        return !length || write_bytes(cursor, (unsigned)length);
    }
};

static long source_read(void *context, void *buffer, unsigned long count) {
    return sys_read(*(int *)context, buffer, count);
}

static long source_seek(void *context, long offset, int origin) {
    return sys_lseek(*(int *)context, offset, origin);
}

static int fill_strip(const pjpeg_image_info_t *info, unsigned char *strip) {
    unsigned pitch = (unsigned)info->m_width * 3u;
    for (int mcu_x = 0; mcu_x < info->m_MCUSPerRow; ++mcu_x) {
        if (pjpeg_decode_mcu()) return -1;
        for (int block_y = 0; block_y < info->m_MCUHeight; block_y += 8) {
            for (int block_x = 0; block_x < info->m_MCUWidth; block_x += 8) {
                int image_x = mcu_x * info->m_MCUWidth + block_x;
                if (image_x >= info->m_width) continue;
                unsigned offset = (unsigned)(block_x * 8 + block_y * 16);
                int cols = info->m_width - image_x < 8 ? info->m_width - image_x : 8;
                for (int y = 0; y < 8; ++y) {
                    unsigned char *out = strip + (unsigned)(block_y + y) * pitch +
                                         (unsigned)image_x * 3u;
                    for (int x = 0; x < cols; ++x) {
                        unsigned source = offset + (unsigned)(y * 8 + x);
                        unsigned char value = info->m_pMCUBufR[source];
                        out[x * 3] = value;
                        out[x * 3 + 1] = info->m_comps == 1 ? value : info->m_pMCUBufG[source];
                        out[x * 3 + 2] = info->m_comps == 1 ? value : info->m_pMCUBufB[source];
                    }
                }
            }
        }
    }
    return 0;
}

static int preset_index(const char *name) {
    const char *names[]={
        "portra_400","cinestill_800t","fuji_provia","kodak_gold","tri_x_400",
        "classic_chrome","agfa_ultra_100","polaroid_669","fuji_acros","polaroid_px680"
    };
    for (int i=0;i<10;++i) if (!strcmp(name,names[i])) return i;
    return -1;
}
static int decode_begin(int fd,input_state *input,pjpeg_image_info_t *info) {
    if (sys_lseek(fd,0,0)!=0) return -1;
    input->fd=fd; input->failed=0; memset(info,0,sizeof(*info));
    unsigned status=pjpeg_decode_init(info,read_jpeg,input,0);
    if (status || input->failed || info->m_width<1 || info->m_height<1 ||
        (unsigned)info->m_width>MAX_IMAGE_DIMENSION ||
        (unsigned)info->m_height>MAX_IMAGE_DIMENSION ||
        (unsigned long)info->m_width*(unsigned long)info->m_height>MAX_PIXELS ||
        info->m_MCUHeight<1 || info->m_MCUHeight>16 ||
        (info->m_comps!=1 && info->m_comps!=3)) {
        printf("FILM_ERROR decode_init=%u\n",status); return -1;
    }
    return 0;
}
struct workspace {
    unsigned char *strip,*mask,*scratch;
    size_t strip_size,mask_size;
    workspace():strip(NULL),mask(NULL),scratch(NULL),strip_size(0),mask_size(0) {}
    ~workspace() {
        if(strip) xfree(strip,strip_size);
        if(mask) xfree(mask,mask_size);
        if(scratch) xfree(scratch,mask_size);
    }
};
#ifndef A6400_FILM_JOB
static int mask_pass(const pjpeg_image_info_t *info,workspace *work,int preset,int grain) {
    film_state film; film_init(&film,preset,grain);
    for (int row=0;row<info->m_MCUSPerCol;++row) {
        if (fill_strip(info,work->strip)) return -1;
        int y=row*info->m_MCUHeight;
        int rows=info->m_height-y<info->m_MCUHeight?info->m_height-y:info->m_MCUHeight;
        film_pixels(&film,work->strip,(unsigned long)info->m_width*(unsigned)rows);
        film_mask_sample(work->mask,info->m_width,info->m_height,y,work->strip,rows);
    }
    film_mask_blur(work->mask,work->scratch,info->m_width/4,info->m_height/4);
    xfree(work->scratch,work->mask_size); work->scratch=NULL;
    return 0;
}
static int render(int fd,const char *path,int preset,int grain,int halo,
                  const unsigned char *metadata,unsigned metadata_size) {
    input_state input={fd,0}; pjpeg_image_info_t info={};
    if (decode_begin(fd,&input,&info)) return 4;
    int width=info.m_width,height=info.m_height;
    workspace work;
    work.strip_size=(size_t)width*3u*(unsigned)info.m_MCUHeight;
    work.strip=(unsigned char *)xalloc(work.strip_size);
    if (!work.strip) return 5;
    if (halo && preset!=4 && preset!=8 && width/4>=8 && height/4>=8) {
        work.mask_size=(size_t)(width/4)*(unsigned)(height/4);
        work.mask=(unsigned char *)xalloc(work.mask_size);
        work.scratch=(unsigned char *)xalloc(work.mask_size);
        if (!work.mask || !work.scratch) return 5;
        if (mask_pass(&info,&work,preset,grain) || input.failed) return 8;
        printf("FILM_MASK_READY width=%d height=%d\n",width,height);
        if (decode_begin(fd,&input,&info) || info.m_width!=width || info.m_height!=height) return 4;
    }
    int output_fd=sys_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if (output_fd<0) {printf("FILM_ERROR output_open=%d\n",output_fd);return 6;}
    output_stream output(output_fd,metadata,metadata_size);
    jpge::params settings; settings.m_quality=90; settings.m_subsampling=jpge::H2V2;
    jpge::jpeg_encoder encoder;
    int result=encoder.init(&output,width,height,3,settings)?0:7;
    film_state film; film_init(&film,preset,grain);
    for(int row=0;!result && row<info.m_MCUSPerCol;++row) {
        if(fill_strip(&info,work.strip)){result=8;break;}
        int y=row*info.m_MCUHeight;
        int rows=height-y<info.m_MCUHeight?height-y:info.m_MCUHeight;
        film_pixels(&film,work.strip,(unsigned long)width*(unsigned)rows);
        if(work.mask) film_halo_apply(work.strip,width,rows,y,work.mask,width/4,height/4,preset);
        for(int line=0;line<rows;++line)
            if(!encoder.process_scanline(work.strip+(size_t)line*(unsigned)width*3u)){result=10;break;}
    }
    if(!result && !encoder.process_scanline(NULL)) result=11;
    encoder.deinit();
    int flushed=film_fsync(output_fd),closed=sys_close(output_fd);
    if(result || input.failed || output.failed || flushed || closed || !output.bytes) {
        sys_unlink(path);printf("FILM_ERROR render=%d output_failed=%d\n",result,output.failed);return 12;
    }
    printf("FILM_DONE width=%d height=%d bytes=%u strip_kib=%u mask_kib=%u preset=%d grain=%d halation=%d\n",
        width,height,output.bytes,(unsigned)((work.strip_size+1023)/1024),
        (unsigned)((work.mask_size+1023)/1024),preset,grain,halo);
    return 0;
}
#endif
#ifdef A6400_FILM_JOB
#include "author_fast_jpeg.h"
/* Check markers and decode every MCU without allocating a second JPEG or
 * applying film. This is decoder acceptance, not a byte-integrity guarantee:
 * picojpeg can tolerate corrupt entropy followed by an early EOI. Native copy
 * size/hash and SD readback checks remain the caller's separate responsibility.
 * The caller opens the input read-only and supplies no output pathname. */
static int verify_decode(int fd) {
    input_state input={fd,0}; pjpeg_image_info_t info={};
    if (decode_begin(fd,&input,&info)) return 4;
    workspace work;
    work.strip_size=(size_t)info.m_width*3u*(unsigned)info.m_MCUHeight;
    work.strip=(unsigned char *)xalloc(work.strip_size);
    if (!work.strip) return 5;
    for (int row=0;row<info.m_MCUSPerCol;++row)
        if (fill_strip(&info,work.strip) || input.failed) {
            printf("FILM_ERROR verify_decode\n");return 8;
        }
    if (pjpeg_decode_mcu()!=PJPG_NO_MORE_BLOCKS || input.failed) {
        printf("FILM_ERROR verify_incomplete\n");return 8;
    }
    printf("FILM_DECODED width=%d height=%d strip_kib=%u\n",info.m_width,info.m_height,
           (unsigned)((work.strip_size+1023)/1024));
    return 0;
}
#endif
int main(int argc,char **argv) {
    static unsigned char metadata[512u*1024u]; unsigned long metadata_size=0;
    if(argc==2 && !strcmp(argv[1],"--list")) {
        printf("FILM_READY portra_400 cinestill_800t fuji_provia kodak_gold tri_x_400 classic_chrome agfa_ultra_100 polaroid_669 fuji_acros polaroid_px680\n");return 0;
    }
#ifdef A6400_FILM_JOB
    if(argc==5 && !strcmp(argv[1],"--album-preview")) {
        int rc=author_album_preview_path(argv[2],argv[3],argv[4]);
        if(rc){printf("FILM_ALBUM_PREVIEW_FAIL rc=%d\n",rc);return 1;}
        printf("FILM_ALBUM_PREVIEW_OK\n");
        return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--verify")) {
#ifndef A6400_FILM_HOST
        if(!film_input_path(argv[2])) {printf("FILM_ERROR path_policy\n");return 1;}
#endif
        int fd=sys_open(argv[2],O_RDONLY|O_NOFOLLOW,0);
        if(fd<0){printf("FILM_ERROR input_open=%d\n",fd);return 2;}
        int result;
        if(!jpeg_first_frame_valid(&fd,source_read,source_seek,MAX_INPUT_BYTES,
                                  metadata,sizeof(metadata),&metadata_size)) {
            printf("FILM_ERROR input_markers_or_size\n");result=3;
        } else result=verify_decode(fd);
        int closed=sys_close(fd);
        return result?result:closed?2:0;
    }
    if(argc>=6 && !strcmp(argv[1],"--dexp")) {
        int bmode=!strcmp(argv[2],"screen")?(int)A6400_DEXP_SCREEN:
                  !strcmp(argv[2],"lighten")?(int)A6400_DEXP_LIGHTEN:
                  !strcmp(argv[2],"average")?(int)A6400_DEXP_AVERAGE:0;
        const char *in1=argv[3],*in2=argv[4],*outp=argv[5],*pv=NULL;
        int dhires=0,dargi=6,dbad=(bmode==0);
        while(dargi<argc) {
            if(!strcmp(argv[dargi],"--hires") && !dhires){dhires=1;++dargi;}
            else if(!strcmp(argv[dargi],"--ultra12m") && !dhires){dhires=2;++dargi;}
            else if(!strcmp(argv[dargi],"--preview") && !pv && dargi+1<argc){pv=argv[dargi+1];dargi+=2;}
            else{dbad=1;break;}
        }
        if(dbad || !strcmp(in1,outp) || !strcmp(in2,outp) || !strcmp(in1,in2)) {
            printf("usage: film_jpeg --dexp screen|lighten|average INPUT1 INPUT2 OUTPUT [--hires|--ultra12m] [--preview PATH]\n");
            return 1;
        }
#ifndef A6400_FILM_HOST
        if(!film_input2_path(in1) || !film_input_path(in2) || !film_output_path(outp) ||
           (pv && !film_preview_path(pv))) {
            printf("FILM_ERROR path_policy\n");return 1;
        }
#endif
        int fd1=sys_open(in1,O_RDONLY|O_NOFOLLOW,0);
        if(fd1<0){printf("FILM_ERROR input_open=%d\n",fd1);return 2;}
        if(!jpeg_first_frame_valid(&fd1,source_read,source_seek,MAX_INPUT_BYTES,
                                  metadata,sizeof(metadata),&metadata_size)) {
            sys_close(fd1);printf("FILM_ERROR input_markers_or_size\n");return 3;
        }
        int fd2=sys_open(in2,O_RDONLY|O_NOFOLLOW,0);
        if(fd2<0){sys_close(fd1);printf("FILM_ERROR input_open=%d\n",fd2);return 2;}
        if(!jpeg_first_frame_valid(&fd2,source_read,source_seek,MAX_INPUT_BYTES,
                                  metadata,sizeof(metadata),&metadata_size)) {
            sys_close(fd2);sys_close(fd1);printf("FILM_ERROR input_markers_or_size\n");return 3;
        }
        int dres=render_author_dexp(fd1,fd2,outp,bmode,metadata,(unsigned)metadata_size,dhires,pv);
        sys_close(fd2);sys_close(fd1);
        return dres;
    }
#endif
    int preset=argc>=2?preset_index(argv[1]):-1,grain=2,halo=1,hires=0;
    const char *preview_path=NULL;
    int argi=4, bad_args=0;
    if(argc<4) bad_args=1;
    else {
        if(argi+1<argc && strlen(argv[argi])==1 && argv[argi][0]>='0' && argv[argi][0]<='3' &&
           strlen(argv[argi+1])==1 && (argv[argi+1][0]=='0' || argv[argi+1][0]=='1')) {
            grain=argv[argi][0]-'0';
            halo=argv[argi+1][0]-'0';
            argi+=2;
        }
        while(argi<argc) {
            if(!strcmp(argv[argi],"--hires") && !hires) { hires=1; ++argi; }
            else if(!strcmp(argv[argi],"--ultra12m") && !hires) { hires=2; ++argi; }
            else if(!strcmp(argv[argi],"--preview") && !preview_path && argi+1<argc) {
                preview_path=argv[argi+1]; argi+=2;
            } else { bad_args=1; break; }
        }
    }
    if(bad_args||preset<0||grain<0||grain>3||halo<0||halo>1) {
        printf("usage: film_jpeg PRESET INPUT NEW_OUTPUT [GRAIN_0_TO_3 HALATION_0_OR_1] [--hires|--ultra12m] [--preview PATH]\n");return 1;
    }
    if(!strcmp(argv[2],argv[3])) return 1;
#ifndef A6400_FILM_HOST
    if(!film_input_path(argv[2]) || !film_output_path(argv[3]) ||
       (preview_path && !film_preview_path(preview_path))) {
        printf("FILM_ERROR path_policy\n");return 1;
    }
#endif
    int fd=sys_open(argv[2],O_RDONLY|O_NOFOLLOW,0);
    if(fd<0){printf("FILM_ERROR input_open=%d\n",fd);return 2;}
    if(!jpeg_first_frame_valid(&fd,source_read,source_seek,MAX_INPUT_BYTES,
                              metadata,sizeof(metadata),&metadata_size)) {
        sys_close(fd);printf("FILM_ERROR input_markers_or_size\n");return 3;
    }
#ifdef A6400_FILM_JOB
    int result=render_author_fast(fd,argv[3],preset,grain,halo,metadata,(unsigned)metadata_size,hires,preview_path);
#else
    (void)preview_path;
    int result=render(fd,argv[3],preset,grain,halo,metadata,(unsigned)metadata_size);
#endif
    sys_close(fd);return result;
}

