/* One bounded IFSYS command through the existing Sony monitor. No direct Sony
 * calls, process-memory writes, SD mount or UI changes. NATIVE_RETURN means
 * only that a fresh exact command echo and following prompt were observed;
 * copied bytes must be verified independently by the caller.
 */
#define _GNU_SOURCE
#include "film_native_command_build.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FILM_NATIVE_STARTUP_MS
#define FILM_NATIVE_STARTUP_MS 350u
#endif
#ifndef FILM_NATIVE_COMMAND_MS
#define FILM_NATIVE_COMMAND_MS 60000u
#endif
#ifndef FILM_NATIVE_QUIET_MS
#define FILM_NATIVE_QUIET_MS 100u
#endif
#include "im_pid_find.h"
#ifndef FILM_NATIVE_STARTUP_MAX_MS
#define FILM_NATIVE_STARTUP_MAX_MS 3000u
#endif
#ifndef FILM_NATIVE_SOUND_INPUT
#define FILM_NATIVE_SOUND_INPUT "/tmp_bt/a6400-custom-charo.pcm"
#endif
static char film_native_proc_comm[64], film_native_proc_maps[64], film_native_proc_mem[64];
__attribute__((unused)) static void film_native_init_proc_paths(void) {
    if (a6400_im_proc_path(film_native_proc_comm, sizeof(film_native_proc_comm), "comm") ||
        a6400_im_proc_path(film_native_proc_maps, sizeof(film_native_proc_maps), "maps") ||
        a6400_im_proc_path(film_native_proc_mem, sizeof(film_native_proc_mem), "mem")) {
        snprintf(film_native_proc_comm, sizeof(film_native_proc_comm), "/proc/0/comm");
        snprintf(film_native_proc_maps, sizeof(film_native_proc_maps), "/proc/0/maps");
        snprintf(film_native_proc_mem, sizeof(film_native_proc_mem), "/proc/0/mem");
    }
}
#ifndef FILM_NATIVE_PROC_COMM
#define FILM_NATIVE_PROC_COMM (film_native_proc_comm[0] ? film_native_proc_comm : (film_native_init_proc_paths(), film_native_proc_comm))
#endif
#ifndef FILM_NATIVE_PROC_MAPS
#define FILM_NATIVE_PROC_MAPS (film_native_proc_maps[0] ? film_native_proc_maps : (film_native_init_proc_paths(), film_native_proc_maps))
#endif
#ifndef FILM_NATIVE_PROC_MEM
#define FILM_NATIVE_PROC_MEM (film_native_proc_mem[0] ? film_native_proc_mem : (film_native_init_proc_paths(), film_native_proc_mem))
#endif
#ifndef RESPONSE_BYTES
#define RESPONSE_BYTES 32768u
#endif
#ifndef HISTORY_BYTES
#define HISTORY_BYTES 131072u
#endif
#define ECHO_LINE_BYTES 1024u

struct transport { pid_t pid; int input,output; };
static volatile sig_atomic_t stopping;
static char response[RESPONSE_BYTES+1];
static unsigned startup_received,pre_echo_received,pre_echo_overflows;

static void stop_signal(int sig) { stopping=sig; }
static int clock_ms(uint32_t *out) {
    struct timespec t;
#ifdef __APPLE__
    if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;
#else
    if(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t))return -1;
#endif
    *out=(uint32_t)((uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u);
    return 0;
}
static void pause_ms(unsigned ms) { (void)poll(NULL,0,(int)ms); }

static int safe_name(const char *p,unsigned count) {
    for(unsigned i=0;i<count;++i)
        if(!((p[i]>='A'&&p[i]<='Z')||(p[i]>='a'&&p[i]<='z')||
             (p[i]>='0'&&p[i]<='9')||p[i]=='_'||p[i]=='-'))return 0;
    return 1;
}
static int capture_path(const char *p) {
    return strlen(p)==29 && !memcmp(p,"A:/DCIM/",8) && p[16]=='/' &&
        safe_name(p+8,8) && safe_name(p+17,8) && !strcmp(p+25,".JPG");
}
static int output_path(const char *p) {
    if(strlen(p)!=26 || memcmp(p,"A:/A6400/TEST/F",15) || strcmp(p+22,".JPG"))return 0;
    for(unsigned i=15;i<22;++i)if(p[i]<'0'||p[i]>'9')return 0;
    return 1;
}
static int sound_path(const char *p) {
    if(!strcmp(p,"A:/A6400/CHARO.PCM") || !strcmp(p,"A:/A6400/SHUTTER.PCM"))return 1;
    if(strncmp(p,"A:/A6400/SOUNDS/",16)!=0)return 0;
    const char *name=p+16;
    size_t n=strlen(name);
    if(n<5u || n>44u || name[0]=='.')return 0;
    const char *ext=name+n-4u;
    if(!(ext[0]=='.' && (ext[1]=='p'||ext[1]=='P') &&
         (ext[2]=='c'||ext[2]=='C') && (ext[3]=='m'||ext[3]=='M')))return 0;
    for(size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)name[i];
        if(c<=0x20u || c=='/' || c=='\\' || c=='"' || c=='\'' || c==';' ||
           c=='$' || c=='`' || c=='|' || c=='&' || c=='<' || c=='>' ||
           c=='*' || c=='?')return 0;
    }
    return 1;
}
static int usr_ro_mounted(void) {
    char buf[4096];
    int fd=open("/proc/mounts",O_RDONLY);
    if(fd<0)return 0;
    ssize_t n=read(fd,buf,sizeof(buf)-1u);
    close(fd);
    if(n<=0)return 0;
    buf[n]=0;
    for(char *p=buf;*p;) {
        char *nl=strchr(p,'\n');
        if(nl)*nl=0;
        char *sp1=strchr(p,' ');
        if(sp1 && !strncmp(sp1+1,"/usr ",5)) {
            char *sp2=strchr(sp1+6,' ');
            if(sp2 && (!strncmp(sp2+1,"ro,",3) || !strncmp(sp2+1,"ro ",3)))return 1;
        }
        if(!nl)break;
        p=nl+1;
    }
    return 0;
}
static int elf32_arm_exec(const unsigned char *eh,unsigned len) {
    if(len<52u)return 0;
    if(eh[0]!=0x7fu || eh[1]!='E' || eh[2]!='L' || eh[3]!='F')return 0;
    if(eh[4]!=1u || eh[5]!=1u)return 0; /* ELFCLASS32, ELFDATA2LSB */
    unsigned etype=(unsigned)eh[16]|((unsigned)eh[17]<<8);
    unsigned emach=(unsigned)eh[18]|((unsigned)eh[19]<<8);
    return (etype==2u || etype==3u) && emach==40u; /* ET_EXEC/ET_DYN, EM_ARM */
}
static int fd_meta(int fd,uint64_t *dev,uint32_t *mode,uint32_t *uid) {
#if defined(__linux__) && defined(__arm__)
    unsigned char raw[128];
    memset(raw,0,sizeof(raw));
    if(syscall(SYS_fstat64,fd,raw)!=0)return -1;
    memcpy(dev,raw+0,8);
    memcpy(mode,raw+16,4);
    memcpy(uid,raw+24,4);
    return 0;
#else
    struct stat st;
    if(fstat(fd,&st)!=0)return -1;
    *dev=(uint64_t)st.st_dev;*mode=(uint32_t)st.st_mode;*uid=(uint32_t)st.st_uid;
    return 0;
#endif
}
static int path_dev(const char *path,uint64_t *dev) {
#if defined(__linux__) && defined(__arm__)
    unsigned char raw[128];
    memset(raw,0,sizeof(raw));
    if(syscall(SYS_stat64,path,raw)!=0)return -1;
    memcpy(dev,raw+0,8);
    return 0;
#else
    struct stat st;
    if(stat(path,&st)!=0)return -1;
    *dev=(uint64_t)st.st_dev;
    return 0;
#endif
}
static int monitor_matches(void) {
    unsigned char block[1024],ehdr[52];
    uint32_t hash=2166136261u,total=0;
    unsigned ehdr_have=0;
    int fd=open(FILM_NATIVE_MONITOR,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return 0;
    uint64_t fdev=0,udev=0;uint32_t fmode=0,fuid=1;
    int stat_ok=(fd_meta(fd,&fdev,&fmode,&fuid)==0);
    ssize_t n;
    while((n=read(fd,block,sizeof(block)))>0) {
        if(total>262144u || (unsigned)n>262144u-total) {
            close(fd);return 0;
        }
        if(ehdr_have<sizeof(ehdr)) {
            unsigned take=(unsigned)sizeof(ehdr)-ehdr_have;
            if((unsigned)n<take)take=(unsigned)n;
            memcpy(ehdr+ehdr_have,block,take);
            ehdr_have+=take;
        }
        for(ssize_t i=0;i<n;++i)hash=(hash^block[i])*16777619u;
        total+=(unsigned)n;
    }
    close(fd);
    if(n!=0)return 0;
    if((total==FILM_NATIVE_MONITOR_BYTES && hash==FILM_NATIVE_MONITOR_FNV) ||
       (total==22648u && hash==0x761681b7u))return 1;
    /* Cross-FW A6400/A6100 fallback: accept any genuine root-owned ARM32 ELF
     * /usr/bin/mwf_monio.elf on Sony's read-only /usr rootfs without rebuild. */
    if(!strcmp(FILM_NATIVE_MONITOR,"/usr/bin/mwf_monio.elf") &&
       stat_ok && S_ISREG(fmode) && fuid==0 && (fmode&0022u)==0 &&
       total>=1024u && total<=262144u && elf32_arm_exec(ehdr,ehdr_have) &&
       path_dev("/usr",&udev)==0 && fdev==udev && usr_ro_mounted()) {
        return 1;
    }
    return 0;
}

static void transport_stop(struct transport *t) {
    if(t->input>=0)close(t->input);
    if(t->output>=0)close(t->output);
    t->input=t->output=-1;
    if(t->pid>0) {
        int status;pid_t got=waitpid(t->pid,&status,WNOHANG);
        if(!got) {
            kill(t->pid,SIGTERM);
            for(unsigned elapsed=0;elapsed<500u;elapsed+=10u) {
                got=waitpid(t->pid,&status,WNOHANG);
                if(got!=0)break;
                pause_ms(10);
            }
            if(!got)kill(t->pid,SIGKILL);
            if(got<=0)while(waitpid(t->pid,&status,0)<0 && errno==EINTR){}
        }
    }
    t->pid=-1;
}
static int transport_start(struct transport *t) {
    int in[2],out[2];
    if(pipe(in))return -1;
    if(pipe(out)){close(in[0]);close(in[1]);return -1;}
    t->pid=fork();
    if(t->pid<0){close(in[0]);close(in[1]);close(out[0]);close(out[1]);return -1;}
    if(!t->pid) {
        if(dup2(in[0],0)<0 || dup2(out[1],1)<0 || dup2(out[1],2)<0)_exit(126);
        close(in[0]);close(in[1]);close(out[0]);close(out[1]);
        execl(FILM_NATIVE_MONITOR,FILM_NATIVE_MONITOR,(char *)0);_exit(127);
    }
    close(in[0]);close(out[1]);t->input=in[1];t->output=out[0];
    return fcntl(t->output,F_SETFL,O_NONBLOCK)<0?-1:0;
}
/* Remove the monitor's pre-existing history before sending anything. Errors in
 * this historical ring are neither a fresh command failure nor a success. */
static int drain_startup(struct transport *t,unsigned startup_ms,unsigned quiet_ms) {
    uint32_t start,now,last;char block[1024];
    if(clock_ms(&start))return -1;
    last=start;
    while(!stopping) {
        if(clock_ms(&now))return -1;
        ssize_t n=read(t->output,block,sizeof(block));
        if(n>0) {
            startup_received+=(unsigned)n;last=now;
            if(startup_received>HISTORY_BYTES)return -3;
        }
        else if(!n)return -1;
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)return -1;
        else {
            if((uint32_t)(now-start)>=startup_ms &&
               (uint32_t)(now-last)>=quiet_ms)return 0;
            pause_ms(5);
        }
        if((uint32_t)(now-start)>=FILM_NATIVE_STARTUP_MAX_MS)return -2;
    }
    return -1;
}

/* An echo is a complete line equal to our command, optionally following an
 * existing MWF> prefix. Overlong lines are not allowed to match by suffix. */
static int echo_line(const char *line,unsigned used,const char *command) {
    const char *p=line,*end=line+used;size_t length=strlen(command);
    if(used>=4u && !memcmp(p,"MWF>",4))p+=4;
    while(p<end && (*p==' '||*p=='\t'))++p;
    return (size_t)(end-p)==length && !memcmp(p,command,length);
}
static int native_error(const char *text) {
    return strstr(text,"Can't open") || strstr(text,"ERROR") ||
        strstr(text,"Fsys_Open err") || strstr(text,"Fsys_Read err") ||
        strstr(text,"Fsys_Write err") || strstr(text,"Fsys_Close err") ||
        strstr(text,"Fsys_Flush err");
}
/* The native copy command does not truncate its Linux destination. A previous
 * or partial input, including a symlink/FIFO/directory, forbids an absence probe.
 * No stat-family ABI is needed with the original camera libc. */
static int path_absent(const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd>=0){close(fd);return 0;}
    return errno==ENOENT;
}
static int input_absent(void) { return path_absent(FILM_NATIVE_INPUT); }
/* An absent result records only this command's source-open failure. It is not
 * proof of ENOENT, atomic exclusive creation, or a reservation of the SD path.
 * Fail closed on any additional diagnostic instead of ignoring another error. */
static int absent_reply(const char *path,const char *command) {
    char expected[96];unsigned count=0;
    int n=snprintf(expected,sizeof(expected),"[IFSYS]Can't open file %s",path);
    if(n<1 || (unsigned)n>=sizeof(expected))return 0;
    const char *line=response;
    while(*line) {
        const char *next=strchr(line,'\n'),*end=next?next:line+strlen(line);
        unsigned size=(unsigned)(end-line);
        if(next && size==(unsigned)n && !memcmp(line,expected,size))++count;
        else if(size && !(size==4u && !memcmp(line,"MWF>",4)) &&
                !(next && echo_line(line,size,command)))return 0;
        if(!next)break;
        line=next+1;
    }
    return count==1u;
}
static int report(int result,int uncertain,const char *reason,const char *operation,unsigned used) {
    printf("%s operation=%s reason=%s response_bytes=%u uncertain=%d copy_verified=0 startup_history_bytes=%u pre_echo_bytes=%u pre_echo_overflows=%u\n",
           result?"NATIVE_FAIL":!strcmp(operation,"absent")?"NATIVE_ABSENT":"NATIVE_RETURN",
           operation,reason,used,uncertain,
           startup_received,pre_echo_received,pre_echo_overflows);
    if(used) {
        unsigned first=used>384u?used-384u:0;
        printf("NATIVE_REPLY_TAIL_HEX ");
        for(unsigned i=first;i<used;++i)printf("%02x",(unsigned)(unsigned char)response[i]);
        printf("\n");
    }
    return result;
}
static int update_overflow(unsigned *match,unsigned char c) {
    static const char token[]="OVERFLOW";
    if(c==(unsigned char)token[*match])++*match;
    else *match=c=='O'?1u:0u;
    if(*match==sizeof(token)-1u){*match=0;return 1;}
    return 0;
}
/* Real Sony mwf_monio writes our command once (`\n`) when queueing it and Sony's
 * MWF task prints it again (`\r\n`) when starting execution; if the first echo
 * arrived on chunk boundaries or matched before queued MWF async logs flushed,
 * re-anchor once on the second exact echo before any closing prompt. */
static int pending_prompt_echo(const char *buf,unsigned used,const char *command,int echoed) {
    if(echoed!=1)return 0;
    const char *mwf=strstr(buf,"MWF>");
    if(!mwf || (mwf!=buf && mwf[-1]!='\n') || strchr(mwf,'\n'))return 0;
    const char *tail=mwf+4;
    while(tail<buf+used && (*tail==' '||*tail=='\t'))++tail;
    size_t len=(size_t)((buf+used)-tail),cmd_len=strlen(command);
    return len>0u && len<=cmd_len && !memcmp(tail,command,len);
}
static int exchange(struct transport *t,const char *command,const char *operation,int flushing,const char *absence_path,const char *read_source,int report_on_ok) {
    uint32_t start,now;unsigned used=0,received=0;size_t sent=0;
    char line[ECHO_LINE_BYTES];unsigned line_used=0,overflow_match=0;
    unsigned resp_line_start=0,resp_overflow_match=0;
    int echoed=0,long_line=0,resp_long_line=0,resp_overflowed=0;
    char request[512];int n=snprintf(request,sizeof(request),"%s\n",command);
    if(n<1 || (unsigned)n>=sizeof(request) || clock_ms(&start))
        return report(6,0,"before_send",operation,0);
    while(sent<(size_t)n && !stopping) {
        ssize_t amount=write(t->input,request+sent,(size_t)n-sent);
        if(amount>0)sent+=(size_t)amount;
        else if(amount<0 && errno==EINTR)continue;
        else return report(sent?21:6,sent!=0,"write_command",operation,0);
    }
    if(stopping)return report(sent?21:20,sent!=0,"signal",operation,0);
    printf("NATIVE_SENT operation=%s command=%s\n",operation,command);
    for(;;) {
        if(stopping)return report(21,1,"signal",operation,used);
        if(clock_ms(&now))return report(21,1,"clock",operation,used);
        if((uint32_t)(now-start)>=FILM_NATIVE_COMMAND_MS)
            return report(21,1,"deadline",operation,used);
        char block[1024];ssize_t amount=read(t->output,block,sizeof(block));
        if(amount>0) {
            for(ssize_t i=0;i<amount;++i) {
                unsigned char c=(unsigned char)block[i];
                if(!c)c='?';
                if(!echoed) {
                    /* History can still arrive after we send. Retain just one
                     * bounded line while finding the exact echo, not an ever
                     * growing response. Old OVERFLOW is counted, not promoted
                     * into a failure of a command whose echo has not appeared. */
                    if(++pre_echo_received>HISTORY_BYTES-startup_received)
                        return report(21,1,"history_capacity",operation,used);
                    if(update_overflow(&overflow_match,c))++pre_echo_overflows;
                    if(c=='\r')continue;
                    if(c=='\n') {
                        if(!long_line && echo_line(line,line_used,command))echoed=1;
                        line_used=0;long_line=0;
                    } else if(line_used<ECHO_LINE_BYTES)line[line_used++]=(char)c;
                    else long_line=1;
                    continue;
                }
                if(update_overflow(&resp_overflow_match,c))resp_overflowed=1;
                if(++received>RESPONSE_BYTES)
                    return report(21,1,"response_capacity",operation,used);
                if(c=='\r')continue;
                if(c=='\n') {
                    if(echoed==1 && !resp_overflowed && !resp_long_line) {
                        char saved=response[resp_line_start];
                        response[resp_line_start]=0;
                        int prior_prompt=strstr(response,"MWF>")!=NULL;
                        response[resp_line_start]=saved;
                        if(!prior_prompt &&
                           echo_line(response+resp_line_start,used-resp_line_start,command)) {
                            if(received>HISTORY_BYTES-startup_received-pre_echo_received)
                                return report(21,1,"history_capacity",operation,used);
                            pre_echo_received+=received;
                            used=0;received=0;response[0]=0;
                            resp_line_start=0;resp_long_line=0;echoed=2;
                            continue;
                        }
                    }
                    response[used++]=(char)c;
                    resp_line_start=used;resp_long_line=0;
                    continue;
                }
                if(used-resp_line_start>=ECHO_LINE_BYTES)resp_long_line=1;
                response[used++]=(char)c;
            }
            response[used]=0;
            if(resp_overflowed || strstr(response,"OVERFLOW"))
                return report(21,1,"ring_overflow",operation,used);
            if(echoed && !pending_prompt_echo(response,used,command,echoed) && strstr(response,"MWF>")) {
                if(absence_path) {
                    if(!input_absent())return report(1,0,"input_not_absent",operation,used);
                    if(!absent_reply(absence_path,command))
                        return report(1,0,"absence_unproven",operation,used);
                    return report_on_ok?report(0,0,"source_open_failed",operation,used):0;
                }
                /* The pinned IFSYS cp opens its source before its destination.
                 * Only this exact fresh source-open failure, with no RAM input,
                 * permits waiting for native media readiness. Never infer this
                 * from another error, a partial copy, readback, or an SD write. */
                if(read_source &&
                   path_absent(!strcmp(operation,"sound")?FILM_NATIVE_SOUND_INPUT:FILM_NATIVE_INPUT) &&
                   absent_reply(read_source,command))
                    return report(14,0,"source_unavailable",operation,used);
                if(native_error(response))return report(1,0,"native_error",operation,used);
                if(flushing && !strstr(response,"Fsys_Flush OK"))
                    return report(1,0,"flush_success_missing",operation,used);
                return report_on_ok?report(0,0,"fresh_prompt",operation,used):0;
            }
        } else if(!amount)return report(21,1,"monitor_eof",operation,used);
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)
            return report(21,1,"read_response",operation,used);
        else pause_ms(5);
    }
}

static int read_mem32(int fd,uint32_t addr,void *buf,size_t len) {
    return addr>=0x10000u && addr<=0x7fffffffu-len &&
        lseek(fd,(off_t)addr,SEEK_SET)==(off_t)addr &&
        read(fd,buf,len)==(ssize_t)len?0:-1;
}
static char mmca_drive_letter(void) {
    static const unsigned char open_prefix[16]={
        0x2d,0xe9,0xf0,0x4f,0x89,0x46,0xdf,0xf8,
        0x20,0x18,0xa9,0xb0,0x04,0x46,0x01,0x20
    };
    char line[256];
    int fd=open(FILM_NATIVE_PROC_COMM,O_RDONLY);
    if(fd<0)return 'A';
    ssize_t n=read(fd,line,16);
    close(fd);
    if(n!=7 || memcmp(line,"im.elf\n",7))return 'A';
    fd=open(FILM_NATIVE_PROC_MAPS,O_RDONLY);
    if(fd<0)return 0;
    uint32_t base=0;unsigned pos=0;
    while(!base && (n=read(fd,response,RESPONSE_BYTES))>0) {
        for(ssize_t i=0;i<n;++i) {
            char c=response[i];
            if(c=='\n') {
                line[pos]=0;
                if(pos>=56u && line[8]=='-' && !memcmp(line+18,"r-xp 00000000",13) &&
                   strstr(line,"/usr/lib/libInfraFsys.so")) {
                    uint32_t v=0;unsigned k;
                    for(k=0;k<8u;++k) {
                        char h=line[k];
                        unsigned d=h>='0'&&h<='9'?(unsigned)(h-'0'):
                                   h>='a'&&h<='f'?(unsigned)(h-'a'+10):16u;
                        if(d>=16u)break;
                        v=(v<<4)|d;
                    }
                    if(k==8u){base=v;break;}
                }
                pos=0;
            } else if(pos+1u<sizeof(line))line[pos++]=c;
        }
    }
    close(fd);
    if(!base)return 0;
    fd=open(FILM_NATIVE_PROC_MEM,O_RDONLY);
    if(fd<0)return 0;
    uint32_t tp=0,sp=0,drives[52];uint16_t st=0;char letter=0;
    if(!read_mem32(fd,base+0x1bf84u,line,16) && !memcmp(line,open_prefix,16) &&
       !read_mem32(fd,base+0x6bff0u,&tp,4) && !read_mem32(fd,base+0x6c034u,&sp,4) &&
       !read_mem32(fd,sp,&st,2) && (st&1u) && !read_mem32(fd,tp,drives,sizeof(drives))) {
        uint32_t drv=drives[45];unsigned char dl=0;
        if(drv && !read_mem32(fd,drv+0xbu,&dl,1) && dl>='A' && dl<='Z' &&
           drives[dl-'A']==drv && !read_mem32(fd,drv+0x954u,line,10) &&
           !memcmp(line,"/dev/mmca",10))letter=(char)dl;
        else {
            for(unsigned i=0;i<26u;++i) {
                if(drives[i] && !read_mem32(fd,drives[i]+0x954u,line,10) &&
                   !memcmp(line,"/dev/mmca",10)){letter=(char)('A'+i);break;}
            }
        }
    }
    close(fd);
    return letter;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
#ifdef FILM_NATIVE_BOOT_STAGE_PATH
    /* A separately built boot adapter has no user-selected operation/path.
     * It can only read one pinned SD bundle into its otherwise absent RAM file.
     * Photo-runtime builds do not expose this operation. */
    (void)argv;
    if(argc!=1)return report(2,0,"arguments","none",0);
    const char *operation="stage",*raw_path=FILM_NATIVE_BOOT_STAGE_PATH;
#else
    if(argc!=3)return report(2,0,"arguments","none",0);
    const char *operation=argv[1],*raw_path=argv[2];
#endif
    int is_stage=0,is_read=0,is_sound=0,is_write=0,is_flush=0,is_absent=0,is_readback=0,is_commit=0;
#ifdef FILM_NATIVE_BOOT_STAGE_PATH
    is_stage=!strcmp(operation,"stage");
#endif
    is_read=!strcmp(operation,"read") && capture_path(raw_path);
    is_sound=!strcmp(operation,"sound") && sound_path(raw_path);
    is_readback=!strcmp(operation,"readback") && output_path(raw_path);
    is_absent=!strcmp(operation,"absent") && output_path(raw_path);
    is_write=!strcmp(operation,"write") && output_path(raw_path);
    is_flush=!strcmp(operation,"flush") && output_path(raw_path);
    is_commit=!strcmp(operation,"commit") && output_path(raw_path);
    if(!(is_stage || is_read || is_sound || is_readback || is_absent || is_write || is_flush || is_commit))
        return report(2,0,"operation_or_path","none",0);
    if((is_stage || is_absent || is_commit) && !input_absent())
        return report(1,0,"input_not_absent",operation,0);
    if(is_sound && !path_absent(FILM_NATIVE_SOUND_INPUT))
        return report(1,0,"input_not_absent",operation,0);
    if(!monitor_matches())return report(3,0,"monitor_pin",operation,0);
    char drv=mmca_drive_letter();
    if(!drv) {
        if(is_stage || (is_read && input_absent()) || (is_sound && path_absent(FILM_NATIVE_SOUND_INPUT)))
            return report(14,0,"source_unavailable",operation,0);
        return report(6,0,"before_send",operation,0);
    }
    char path_buf[64],command[512],cmd_write[512],cmd_flush[512];
    if(snprintf(path_buf,sizeof(path_buf),"%s",raw_path)<2 ||
       strlen(raw_path)>=sizeof(path_buf))
        return report(2,0,"operation_or_path","none",0);
    path_buf[0]=drv;
    const char *path=path_buf;
    int flushing=is_flush,n=-1;
    if(is_write)n=snprintf(command,sizeof(command),"IFSYS cp %s %s",FILM_NATIVE_OUTPUT,path);
    else if(is_flush)n=snprintf(command,sizeof(command),"IFSYS flush %s",path);
    else if(is_sound)n=snprintf(command,sizeof(command),"IFSYS cp %s %s",path,FILM_NATIVE_SOUND_INPUT);
    else n=snprintf(command,sizeof(command),"IFSYS cp %s %s",path,FILM_NATIVE_INPUT);
    if(n<1 || (unsigned)n>=sizeof(command))return report(2,0,"operation_or_path","none",0);
    if(is_commit) {
        int nw=snprintf(cmd_write,sizeof(cmd_write),"IFSYS cp %s %s",FILM_NATIVE_OUTPUT,path);
        int nf=snprintf(cmd_flush,sizeof(cmd_flush),"IFSYS flush %s",path);
        if(nw<1 || (unsigned)nw>=sizeof(cmd_write) || nf<1 || (unsigned)nf>=sizeof(cmd_flush))
            return report(2,0,"operation_or_path","none",0);
    }
    const char *absence_path=(is_absent || is_commit)?path:NULL;
    signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);
    struct transport t={-1,-1,-1};
    if(transport_start(&t)) {
        transport_stop(&t);return report(5,0,"startup",operation,0);
    }
    unsigned drain_ms=is_commit?(FILM_NATIVE_STARTUP_MS<60u?FILM_NATIVE_STARTUP_MS:60u):FILM_NATIVE_STARTUP_MS;
    unsigned q_ms=is_commit?(FILM_NATIVE_QUIET_MS<25u?FILM_NATIVE_QUIET_MS:25u):FILM_NATIVE_QUIET_MS;
    int drained=drain_startup(&t,drain_ms,q_ms);
    if(drained) {
        transport_stop(&t);
        return report(5,0,drained==-2?"startup_busy":drained==-3?"history_capacity":"startup",operation,0);
    }
    int result;
    if(is_commit) {
        result=exchange(&t,command,operation,0,absence_path,NULL,0);
        if(!result)result=exchange(&t,cmd_write,operation,0,NULL,NULL,0);
        if(!result)result=exchange(&t,cmd_flush,operation,1,NULL,NULL,1);
    } else {
        result=exchange(&t,command,operation,flushing,absence_path,
                        (!strcmp(operation,"read") || !strcmp(operation,"stage") || is_sound)?path:NULL,1);
    }
    /* Killing the monitor client does not cancel or prove termination of a
     * Sony in-process operation; uncertain=1 remains authoritative for caller. */
    transport_stop(&t);
    return result;
}
