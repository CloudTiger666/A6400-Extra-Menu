/* Finite, read-only MPR directory observer. Never copies or writes a photo.
 * Stable size/time is a candidate heuristic, not a Sony close/DCF-commit event.
 * No UI, ptrace, input capture, Linux SD mount or injected Sony calls.
 */
#define _GNU_SOURCE
#include "capture_probe_build.h"
#include <ctype.h>
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
#ifndef CAPTURE_STARTUP_MS
#define CAPTURE_STARTUP_MS 1000u
#endif
#ifndef CAPTURE_COMMAND_MS
#define CAPTURE_COMMAND_MS 4000u
#endif
#ifndef CAPTURE_WAIT_MS
#define CAPTURE_WAIT_MS 25000u
#endif
#ifndef CAPTURE_ACTIVE_MS
#define CAPTURE_ACTIVE_MS 600000u
#endif
#ifndef CAPTURE_INTERVAL_MS
#define CAPTURE_INTERVAL_MS 3000u
#endif
#ifndef CAPTURE_STABLE_MS
#define CAPTURE_STABLE_MS 3000u
#endif
#ifndef CAPTURE_QUIET_MS
#define CAPTURE_QUIET_MS 200u
#endif
#ifndef CAPTURE_STARTUP_MAX_MS
#define CAPTURE_STARTUP_MAX_MS 3000u
#endif
#ifndef MAX_FILES
#define MAX_FILES 128u
#endif
#ifndef CAPTURE_STOP_GRACE_MS
#define CAPTURE_STOP_GRACE_MS 80u
#endif
#define MAX_DIRS 8u
#define RESPONSE_BYTES 8192u
#ifndef SCAN_MS
#define SCAN_MS 15000u
#endif
#define PREAMBLE_BYTES 131072u
#define FRAME_LINE_BYTES 256u
struct entry { char path[80],time[32]; unsigned size; };
struct item { char name[64],time[32]; unsigned size; int directory; };
struct catalogue { unsigned count; struct entry items[MAX_FILES]; };
struct seen { struct entry value; uint32_t stable; int reported,present; };
struct transport {
    pid_t pid; int in,out; uint32_t scan_started,command_started;
    unsigned command_seq,response_bytes,tail_bytes,preamble_bytes,body_bytes,overflow_match;
    int header_seen,prompt_after_header,echo_seen,metadata_retryable;
    char command[128],expected[40]; unsigned char tail[384];
    unsigned body_prefix_bytes;unsigned char body_prefix[768];
};
static volatile sig_atomic_t stopping;
static unsigned candidates;
static void stop_signal(int sig) { stopping=sig; }
static uint32_t now_ms(void) {
    struct timespec t;
#ifdef __APPLE__
    if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
#else
    if(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)) return 0;
#endif
    return (uint32_t)((uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u);
}
static void pause_ms(unsigned ms) { (void)poll(NULL,0,(int)ms); }
/* Keep only a bounded byte tail before the parser modifies its buffer. Hex
 * encoding prevents unsolicited native text from injecting terminal controls.
 * Diagnostics do not retry a command or change the shared enumeration cursor. */
static void response_context(struct transport *t,const char *out,unsigned used) {
    t->body_bytes=used;
    t->body_prefix_bytes=used<sizeof(t->body_prefix)?used:sizeof(t->body_prefix);
    memcpy(t->body_prefix,out,t->body_prefix_bytes);
}
static void trace_block(struct transport *t,const char *block,unsigned count) {
    t->response_bytes+=count;
    if(count>=sizeof(t->tail)) {
        memcpy(t->tail,block+count-sizeof(t->tail),sizeof(t->tail));
        t->tail_bytes=sizeof(t->tail);
    } else {
        unsigned keep=t->tail_bytes;
        if(keep+count>sizeof(t->tail))keep=sizeof(t->tail)-count;
        memmove(t->tail,t->tail+t->tail_bytes-keep,keep);
        memcpy(t->tail+keep,block,count);t->tail_bytes=keep+count;
    }
}
static int overflow_byte(unsigned *matched,unsigned char c) {
    static const char token[]="OVERFLOW";
    if(c==(unsigned char)token[*matched])++*matched;
    else *matched=c=='O'?1u:0u;
    if(*matched==sizeof(token)-1u){*matched=0;return 1;}
    return 0;
}
static int diagnostic(struct transport *t,const char *reason,int error) {
    uint32_t now=now_ms();
    printf("CAPTURE_DIAG reason=%s command_seq=%u command_elapsed_ms=%u scan_elapsed_ms=%u errno=%d response_bytes=%u header_seen=%d prompt_after_header=%d command_hex=",
           reason,t->command_seq,(unsigned)(now-t->command_started),
           (unsigned)(now-t->scan_started),error,t->response_bytes,t->header_seen,t->prompt_after_header);
    for(const unsigned char *s=(const unsigned char *)t->command;*s;++s)printf("%02x",(unsigned)*s);
    printf(" response_tail_hex=");
    for(unsigned i=0;i<t->tail_bytes;++i)printf("%02x",(unsigned)t->tail[i]);
    printf(" uptime_ms=%u preamble_bytes=%u body_bytes=%u echo_seen=%d\n",
           (unsigned)now,t->preamble_bytes,t->body_bytes,t->echo_seen);
    /* The raw read tail can be entirely post-prompt async text. Preserve a
     * bounded pre-parse body prefix separately, before strtok edits it. Its
     * hex record fits the runtime logger's 2048-byte per-record limit. */
    if(t->prompt_after_header && t->body_prefix_bytes) {
        printf("CAPTURE_BODY command_seq=%u body_bytes=%u prefix_bytes=%u truncated=%u prefix_hex=",
               t->command_seq,t->body_bytes,t->body_prefix_bytes,
               t->body_prefix_bytes<t->body_bytes?1u:0u);
        for(unsigned i=0;i<t->body_prefix_bytes;++i)printf("%02x",(unsigned)t->body_prefix[i]);
        putchar('\n');
    }
    return -1;
}
static int exchange_failure(struct transport *t,const char *reason,int error,
                            const char *out,unsigned used) {
    response_context(t,out,used);return diagnostic(t,reason,error);
}
/* A RAM marker is checked only between completed native enumerations. Never
 * interrupt an in-process Sony command merely to end an owner test early. */
static int stop_requested(void) {
    int fd=open(CAPTURE_STOP_FILE,O_RDONLY|O_NONBLOCK|O_NOFOLLOW);
    if(fd>=0){close(fd);return 1;}
    return errno==ENOENT?0:-1;
}
static int probe_usr_ro_mounted(void) {
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
static int probe_elf32_arm_exec(const unsigned char *eh,unsigned len) {
    if(len<52u)return 0;
    if(eh[0]!=0x7fu || eh[1]!='E' || eh[2]!='L' || eh[3]!='F')return 0;
    if(eh[4]!=1u || eh[5]!=1u)return 0;
    unsigned etype=(unsigned)eh[16]|((unsigned)eh[17]<<8);
    unsigned emach=(unsigned)eh[18]|((unsigned)eh[19]<<8);
    return (etype==2u || etype==3u) && emach==40u;
}
static int probe_fd_meta(int fd,uint64_t *dev,uint32_t *mode,uint32_t *uid) {
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
static int probe_path_dev(const char *path,uint64_t *dev) {
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
    unsigned char b[512],ehdr[52];
    unsigned count=0,ehdr_have=0;
    uint32_t h=2166136261u;
    int fd=open(CAPTURE_MONITOR,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return 0;
    uint64_t fdev=0,udev=0;uint32_t fmode=0,fuid=1;
    int stat_ok=(probe_fd_meta(fd,&fdev,&fmode,&fuid)==0);
    ssize_t n;
    while((n=read(fd,b,sizeof(b)))>0) {
        if(count>262144u || (unsigned)n>262144u-count){close(fd);return 0;}
        if(ehdr_have<sizeof(ehdr)) {
            unsigned take=(unsigned)sizeof(ehdr)-ehdr_have;
            if((unsigned)n<take)take=(unsigned)n;
            memcpy(ehdr+ehdr_have,b,take);
            ehdr_have+=take;
        }
        count+=(unsigned)n;
        for(ssize_t i=0;i<n;++i)h=(h^b[i])*16777619u;
    }
    close(fd);
    if(n!=0)return 0;
    if((count==CAPTURE_MONITOR_BYTES && h==CAPTURE_MONITOR_FNV) ||
       (count==6776u && h==0xa92e7557u))return 1;
    /* Cross-FW A6400/A6100 fallback: accept any genuine root-owned ARM32 ELF
     * /usr/bin/mpr_monio.elf on Sony's read-only /usr rootfs without rebuild. */
    if(!strcmp(CAPTURE_MONITOR,"/usr/bin/mpr_monio.elf") &&
       stat_ok && S_ISREG(fmode) && fuid==0 && (fmode&0022u)==0 &&
       count>=1024u && count<=262144u && probe_elf32_arm_exec(ehdr,ehdr_have) &&
       probe_path_dev("/usr",&udev)==0 && fdev==udev && probe_usr_ro_mounted()) {
        return 1;
    }
    return 0;
}
static void transport_stop(struct transport *t) {
    if(t->in>=0)close(t->in);
    if(t->out>=0)close(t->out);
    if(t->pid>0) {
        kill(t->pid,SIGTERM);
        uint32_t start=now_ms();int status;
        while(waitpid(t->pid,&status,WNOHANG)==0) {
            if((uint32_t)(now_ms()-start)>CAPTURE_STOP_GRACE_MS) {kill(t->pid,SIGKILL);break;}
            pause_ms(5);
        }
        while(waitpid(t->pid,&status,0)<0 && errno==EINTR){}
    }
    t->pid=-1;t->in=t->out=-1;
}
static int transport_start(struct transport *t) {
    int input[2],output[2];
    if(pipe(input))return -1;
    if(pipe(output)){close(input[0]);close(input[1]);return -1;}
    t->pid=fork();
    if(t->pid<0) {close(input[0]);close(input[1]);close(output[0]);close(output[1]);return -1;}
    if(!t->pid) {
        if(dup2(input[0],0)<0 || dup2(output[1],1)<0 || dup2(output[1],2)<0)_exit(126);
        close(input[0]);close(input[1]);close(output[0]);close(output[1]);
        execl(CAPTURE_MONITOR,CAPTURE_MONITOR,(char *)0);_exit(127);
    }
    close(input[0]);close(output[1]);t->in=input[1];t->out=output[0];
    if(fcntl(t->out,F_SETFL,O_NONBLOCK))return -1;
    /* Drain currently available startup/history output before sending. Quiet
     * is bounded and time based, not command-correlated freshness. Subsequent
     * commands are serialized and require known previous completion. */
    uint32_t start=now_ms(),last=start;char drain[1024];
    unsigned bytes=0,overflows=0,overflow_match=0;int ready=0;
    while(!stopping) {
        uint32_t now=now_ms();
        ssize_t n=read(t->out,drain,sizeof(drain));
        if(n>0){
            bytes+=(unsigned)n;last=now;
            if(bytes>PREAMBLE_BYTES)break;
            for(ssize_t i=0;i<n;++i)overflows+=(unsigned)overflow_byte(&overflow_match,(unsigned char)drain[i]);
        }
        else if(!n)return -1;
        else if(errno!=EAGAIN && errno!=EINTR)return -1;
        else {
            if((uint32_t)(now-start)>=CAPTURE_STARTUP_MS &&
               (uint32_t)(now-last)>=CAPTURE_QUIET_MS){ready=1;break;}
            pause_ms(5);
        }
        if((uint32_t)(now-start)>=CAPTURE_STARTUP_MAX_MS)break;
    }
    printf("CAPTURE_MONITOR_DRAIN bytes=%u startup_overflows=%u ready=%d quiet_ms=%u\n",
           bytes,overflows,ready,(unsigned)(now_ms()-last));
    if(stopping || !ready)return -1;
    /* The drained new stream cannot complete an old stream's partial token.
     * Within this stream the matcher still survives command boundaries. */
    t->overflow_match=0;
    return 0;
}
static int exact_echo(const char *line,unsigned size,const char *command) {
    if(size>=4u && !memcmp(line,"MON>",4)){line+=4;size-=4;}
    while(size && (*line==' '||*line=='\t')){++line;--size;}
    return strlen(command)==size && !memcmp(line,command,size);
}
static int exchange_complete(struct transport *t,char *out,unsigned used,unsigned prompt_start) {
    t->prompt_after_header=1;response_context(t,out,used);
    /* A NUL inside a framed native body means the response was interleaved or
     * corrupted. Keep it as a bounded, whole-inventory retry signal; never
     * let C-string parsing turn the prefix into a valid directory entry. */
    if(memchr(out,'\0',prompt_start)!=NULL)t->metadata_retryable=1;
    out[prompt_start]=0;
    if(t->command_seq==1u || t->preamble_bytes)
        printf("CAPTURE_FRAME command_seq=%u response_bytes=%u preamble_bytes=%u body_bytes=%u echo_seen=%d\n",
               t->command_seq,t->response_bytes,t->preamble_bytes,t->body_bytes,t->echo_seen);
    return 0;
}
static int exchange(struct transport *t,const char *command,const char *header,
                    char *out,unsigned capacity) {
    /* The pinned camera-libmpr monitor calls its synchronous dispatcher at
     * 0x7bd758, then its prompt printer at 0x7bd75c. That printer writes the
     * NUL-terminated MON> bytes, WITHOUT a newline (0x7bd928 -> 0x7bcab8).
     * Thus a complete line-start token ends the frame by itself; arbitrary
     * background text after it is not part of a prompt or native metadata.
     * See host-tests/native-fullsize-job-v1/mpr-prompt-contract-v9.json.
     * This is serialized single-owner framing, not authenticated correlation. */
    unsigned used=0;uint32_t start=now_ms();out[0]=0;
    t->command_started=start;++t->command_seq;
    t->response_bytes=t->tail_bytes=t->preamble_bytes=t->body_bytes=0;
    t->header_seen=t->prompt_after_header=t->echo_seen=0;
    snprintf(t->command,sizeof(t->command),"%s",command);
    snprintf(t->expected,sizeof(t->expected),"%s",header);
    size_t length=strlen(command);ssize_t sent=write(t->in,command,length);
    if(sent!=(ssize_t)length)return exchange_failure(t,"write_command",sent<0?errno:0,out,used);
    if(write(t->in,"\n",1)!=1)return exchange_failure(t,"write_newline",errno,out,used);
    char line[FRAME_LINE_BYTES];unsigned line_used=0,line_bytes=0,prompt_match=0;
    int long_line=0;
    while(!stopping && (uint32_t)(now_ms()-start)<CAPTURE_COMMAND_MS &&
          (uint32_t)(now_ms()-t->scan_started)<SCAN_MS) {
        char block[512];ssize_t n=read(t->out,block,sizeof(block));
        if(n>0) {
            trace_block(t,block,(unsigned)n);
            int overflowed=0;
            for(ssize_t i=0;i<n;++i)
                overflowed|=overflow_byte(&t->overflow_match,(unsigned char)block[i]);
            /* Startup history is drained before send. After send an overflow
             * is always uncertain, including before the expected header. */
            if(overflowed)return exchange_failure(t,"ring_overflow",0,out,used);
            for(ssize_t i=0;i<n;++i) {
                unsigned char c=(unsigned char)block[i];
                if(!t->header_seen) {
                    if(++t->preamble_bytes>PREAMBLE_BYTES)
                        return exchange_failure(t,"preamble_capacity",0,out,used);
                    ++line_bytes;
                    if(c=='\r')continue;
                    if(c=='\n') {
                        if(!long_line && strlen(header)==line_used && !memcmp(line,header,line_used)) {
                            if(line_used+1u>=capacity)return exchange_failure(t,"response_capacity",0,out,used);
                            memcpy(out,line,line_used);used=line_used;out[used++]='\n';
                            t->header_seen=1;t->preamble_bytes-=line_bytes;
                        } else if(!long_line && exact_echo(line,line_used,command))t->echo_seen=1;
                        line_used=line_bytes=0;long_line=0;
                    } else if(line_used<sizeof(line))line[line_used++]=(char)c;
                    else long_line=1;
                    continue;
                }
                if(c=='\r') {
                    /* CRLF before a prompt is normal; CR inside MON> is not
                     * a contiguous delimiter and cannot be stripped into one. */
                    if(prompt_match && prompt_match<4u)prompt_match=5u;
                    continue;
                }
                if(used+1u>=capacity)return exchange_failure(t,"response_capacity",0,out,used);
                out[used++]=(char)c;
                /* Check the WHOLE read block for OVERFLOW before recognizing
                 * a delimiter; retain a partial overflow match across reads
                 * and commands. Discard this block's post-prompt async tail,
                 * never reuse it as another command's header/metadata. */
                if(c=='\n')prompt_match=0;
                else if(prompt_match<4u && c==(unsigned char)"MON>"[prompt_match]) {
                    if(++prompt_match==4u)return exchange_complete(t,out,used,used-4u);
                } else prompt_match=5u;
            }
            out[used]=0;
        } else if(!n)return exchange_failure(t,"read_eof",0,out,used);
        else if(errno!=EAGAIN && errno!=EINTR)return exchange_failure(t,"read_error",errno,out,used);
        else pause_ms(5);
    }
    return exchange_failure(t,stopping?"signal":
             ((uint32_t)(now_ms()-t->scan_started)>=SCAN_MS?"scan_timeout":"command_timeout"),0,out,used);
}
static int component(const char *s) {
    size_t n=strlen(s);if(!n || n>32 || s[0]=='.')return 0;
    for(size_t i=0;i<n;++i)
        if(!((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||
             (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return 0;
    return 1;
}
static int sound_component(const char *s) {
    size_t n=strlen(s);if(!n || n>44u || s[0]=='.')return 0;
    for(size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)s[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
             c=='_'||c=='-'||c=='.'||c=='~'||c>=0x80u))return 0;
    }
    return 1;
}
static int number(const char *s,unsigned *out) {
    unsigned v=0,n=0;
    while(*s==' ')++s;
    while(*s>='0'&&*s<='9') {
        if(v>6710886u || (v==6710886u && *s>'4'))return -1;
        v=v*10u+(unsigned)(*s++-'0');++n;
    }
    while(*s==' ')++s;
    if(!n || *s)return -1;
    *out=v;return 0;
}
static char *trim(char *s) {
    while(*s==' '||*s=='\t')++s;
    size_t n=strlen(s);while(n && (s[n-1]==' '||s[n-1]=='\t'))s[--n]=0;
    return s;
}
static int line_equals(const char *line,size_t bytes,const char *value) {
    return strlen(value)==bytes && !memcmp(line,value,bytes);
}
static int wd_version_line(const char *line,size_t bytes) {
    static const char prefix[]="[MSL][0x",suffix[]="]: MSLF_WD_Version = 0104";
    const size_t begin=sizeof(prefix)-1u,end=sizeof(suffix)-1u;
    if(bytes!=begin+8u+end || memcmp(line,prefix,begin) ||
       memcmp(line+begin+8u,suffix,end))return 0;
    for(size_t i=begin;i<begin+8u;++i)
        if(!isxdigit((unsigned char)line[i]))return 0;
    return 1;
}
/* 1=entry,0=normal end,-1=untrustworthy/incomplete reply,-2=a complete,
 * explicit unmounted-media outcome. The caller must discard the whole partial
 * inventory and start a new root FFirst; never resume an invalidated cursor. */
static int timing_trace_line(const char *s,size_t n) {
    /* Sony's profiler occasionally interleaves `line=836, time=24843, diff=7328`
     * right after `MprFileIf : FFirst [pathname]`. Match the exact numeric shape. */
    size_t i=0u;
    if(n<19u || memcmp(s,"line=",5u))return 0;
    i=5u;
    if(i>=n || !isdigit((unsigned char)s[i]))return 0;
    while(i<n && isdigit((unsigned char)s[i]))++i;
    if(i+7u>n || memcmp(s+i,", time=",7u))return 0;
    i+=7u;
    if(i>=n || !isdigit((unsigned char)s[i]))return 0;
    while(i<n && isdigit((unsigned char)s[i]))++i;
    if(i+7u>n || memcmp(s+i,", diff=",7u))return 0;
    i+=7u;
    if(i>=n || !isdigit((unsigned char)s[i]))return 0;
    while(i<n && isdigit((unsigned char)s[i]))++i;
    return i==n;
}
static int benign_async_debug(const char *s,size_t n) {
    /* Older or AVCHD-populated cards may interleave non-error [AVIMMGR],
     * [MprCnv], or `line=..., time=..., diff=...` profiler lines inside scans.
     * Never treat errors or the explicit DEVICE Unmounted status line as benign. */
    if(timing_trace_line(s,n))return 1;
    if(line_equals(s,n,"[AVIMMGR][MS1]GetDeviceInfo : ER_SEQUENCE  DEVICE Unmounted"))return 0;
    int match=(n>=9u && !memcmp(s,"[AVIMMGR]",9u)) || (n>=8u && !memcmp(s,"[MprCnv]",8u));
    if(!match)return 0;
    static const char *const bad[]={"ERROR","ER_","FAIL","Fail","Unmounted"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        size_t bl=strlen(bad[i]);
        for(size_t pos=0;pos+bl<=n;++pos)
            if(!memcmp(s+pos,bad[i],bl))return 0;
    }
    return 1;
}
static int native_item(struct transport *t,const char *directory,int first,struct item *item) {
    t->metadata_retryable=0;
    char cmd[128],response[RESPONSE_BYTES];const char *verb=first?"FFirst":"FNext";
    if(first)snprintf(cmd,sizeof(cmd),"MPR FileIf FFirst %s/*",directory);
    else strcpy(cmd,"MPR FileIf FNext");
    char expected[40];snprintf(expected,sizeof(expected),"MprFileIf : %s %s",verb,
        first?"[pathname]":"['c' to cancel]");
    if(exchange(t,cmd,expected,response,sizeof(response)))return -1;
    /* exchange removes only its validated final prompt. Statuses are complete
     * trimmed lines and mutually exclusive; a mixed error must never become
     * an empty inventory or a recoverable unmounted-media reply. */
    char *body=response;
    unsigned headers=0,no_more=0,statuses=0,end_status=0,unmounted_status=0;
    unsigned unmounted=0,unmounted_detail=0,unmounted_info_detail=0;
    unsigned first_ok=0,next_ok=0,other=0;
    unsigned retries=0,wd_version=0,getstill=0,cache_notice=0;
    int outcome_started=0,invalid_retry=0,invalid_version=0,invalid_callback=0,invalid_cache=0;
    for(const char *line=body;*line;) {
        const char *next=strchr(line,'\n');size_t bytes=next?(size_t)(next-line):strlen(line);
        const char *value=line;
        while(bytes && (*value==' '||*value=='\t')){++value;--bytes;}
        while(bytes && (value[bytes-1]==' '||value[bytes-1]=='\t'))--bytes;
        if(bytes && ((cache_notice==1u &&
           !line_equals(value,bytes,"[CACHE]ERROR(C1000002h)")) ||
           (cache_notice==2u &&
           !line_equals(value,bytes,"[MprIf] Callback EnterStillPb"))))invalid_cache=1;
        if(!bytes){}
        else if(line_equals(value,bytes,expected))++headers;
        else if(bytes>=5u && !memcmp(value,"[MSL]",5u)) {
            /* Captured on the first cold-media root FFirst. Only this exact
             * version record, once after the header and before any outcome,
             * may precede a complete successful directory entry. */
            if(!first || (strcmp(directory,"ms:/DCIM") && strcmp(directory,"ms:/A6400/TEST") &&
               strcmp(directory,"ms:/A6400/SOUNDS")) ||
               headers!=1u || outcome_started || wd_version++ || !wd_version_line(value,bytes))invalid_version=1;
        }
        else if(bytes>=7u && !memcmp(value,"[CACHE]",7u)) {
            /* Captured during a USB mode transition, ahead of a complete
             * successful FNext entry. Accept only this exact ordered group;
             * an isolated cache error must never become a valid entry. */
            if(first || (strncmp(directory,"ms:/DCIM/",9u) && strcmp(directory,"ms:/A6400/TEST") &&
               strcmp(directory,"ms:/A6400/SOUNDS")) ||
               headers!=1u || outcome_started || getstill)invalid_cache=1;
            if(!cache_notice && line_equals(value,bytes,
                "[CACHE]BUFFER SIZE TOO SMALL : 00000000"))cache_notice=1u;
            else if(cache_notice==1u && line_equals(value,bytes,
                "[CACHE]ERROR(C1000002h)"))cache_notice=2u;
            else invalid_cache=1;
        }
        else if(bytes>=7u && !memcmp(value,"[MprIf]",7u)) {
            /* Actual shooting-mode FNext frame: one exact informational
             * callback between header and filename in a DCIM subdirectory.
             * Never discard arbitrary callbacks, mixed outcomes or suffixes. */
            if(cache_notice==2u && line_equals(value,bytes,"[MprIf] Callback EnterStillPb"))
                cache_notice=3u;
            else if(first || (strncmp(directory,"ms:/DCIM/",9u) && strcmp(directory,"ms:/A6400/TEST") &&
               strcmp(directory,"ms:/A6400/SOUNDS")) ||
               headers!=1u || outcome_started || cache_notice || getstill++ ||
               !line_equals(value,bytes,"[MprIf] Callback GetStillAttrInfo"))invalid_callback=1;
        }
        else if((bytes>=15u && !memcmp(value,"FindFirst Retry",15u)) ||
                (bytes>=14u && !memcmp(value,"FindNext Retry",14u))) {
            /* Sony may announce clearing one old FFirst handle before making
             * the new call. It is informational, never a command retry here.
             * Validate it before any success/end/unmounted early return. */
            if(!first || !line_equals(value,bytes,"FindFirst Retry.") ||
               retries++ || outcome_started)invalid_retry=1;
        } else if(headers==1u && benign_async_debug(value,bytes)) {
            /* Skip benign asynchronous [AVIMMGR]/[MprCnv] trace lines safely. */
        } else {
            outcome_started=1;
            if(line_equals(value,bytes,"No more Files."))++no_more;
            else if(bytes>=2u && !memcmp(value,"ec",2)) {
                ++statuses;
                if(line_equals(value,bytes,"ec = 0x00001008"))++end_status;
                else if(line_equals(value,bytes,"ec = 0x0000100B"))++unmounted_status;
            } else if(line_equals(value,bytes,"FindFirst OK."))++first_ok;
            else if(line_equals(value,bytes,"FindNext OK."))++next_ok;
            else if(line_equals(value,bytes,"[AVIMMGR][MS1]GetDeviceInfo : ER_SEQUENCE  DEVICE Unmounted"))++unmounted;
            else if(line_equals(value,bytes,first?
                "[MMGR][AVI] ## ERROR(MMGR/FileIf):0x0EF10001 ## avimmgr/AviMMgrFileIf.cpp(588) FindFirst()":
                "[MMGR][AVI] ## ERROR(MMGR/FileIf):0x0EF10001 ## avimmgr/AviMMgrFileIf.cpp(696) FindNext()"))++unmounted_detail;
            else if(line_equals(value,bytes,"[MMGR][AVI] ## ERROR(MMGR):0x31000007 ## avimmgr/AviMMgrCmd.cpp(16755) GetDeviceInfo()"))++unmounted_info_detail;
            else ++other;
        }
        if(!next)break;
        line=next+1;
    }
    if(invalid_cache || (cache_notice && cache_notice!=3u))return diagnostic(t,"invalid_cache_notice",0);
    if(invalid_version)return diagnostic(t,"invalid_wd_version",0);
    if(invalid_callback)return diagnostic(t,"invalid_callback",0);
    if(invalid_retry)return diagnostic(t,"invalid_retry",0);
    if(headers!=1u)return diagnostic(t,"duplicate_header",0);
    if(no_more==1u && statuses==1u && end_status==1u && !unmounted_status &&
       !unmounted && !unmounted_detail && !unmounted_info_detail &&
       !first_ok && !next_ok && !other && !wd_version && !getstill && !cache_notice)return 0;
    if(!no_more && statuses==1u &&
       unmounted_status==1u && unmounted==1u && unmounted_detail<=1u &&
       unmounted_info_detail<=1u &&
       !end_status && !first_ok && !next_ok && !other && !wd_version && !getstill && !cache_notice)return -2;
    if(no_more || statuses || unmounted || unmounted_detail || unmounted_info_detail ||
       first_ok!=(first?1u:0u) || next_ok!=(first?0u:1u))return diagnostic(t,"native_status",0);
    /* Only a completed, singly successful response can qualify for a fresh
     * whole-inventory read. Metadata corruption never yields an entry. The
     * daemon may discard the snapshot and retry root FFirst a bounded number
     * of times; transport/native errors and invalid callbacks stay fatal. */
    t->metadata_retryable=1;
    memset(item,0,sizeof(*item));int kind=0,size=0,stamp=0,header=0;
    char *save=NULL;
    for(char *line=strtok_r(body,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        line=trim(line);if(!*line)continue;
        if(strstr(line,expected)==line){if(header++)return diagnostic(t,"duplicate_header",0);continue;}
        if(wd_version && !strncmp(line,"[MSL]",5u))continue; /* Fully validated above. */
        if(getstill && !strcmp(line,"[MprIf] Callback GetStillAttrInfo"))continue;
        if(cache_notice && (!strcmp(line,"[CACHE]BUFFER SIZE TOO SMALL : 00000000") ||
           !strcmp(line,"[CACHE]ERROR(C1000002h)") ||
           !strcmp(line,"[MprIf] Callback EnterStillPb")))continue;
        if(!strcmp(line,"FindFirst Retry."))continue; /* Already position/verb/count validated. */
        if(header && benign_async_debug(line,strlen(line)))continue;
        if(!item->name[0]) {
            int ok_name=!strcmp(directory,"ms:/A6400/SOUNDS")?sound_component(line):component(line);
            if(!ok_name)return diagnostic(t,"invalid_name",0);
            snprintf(item->name,sizeof(item->name),"%s",line);continue;
        }
        if(!strncmp(line,"kind",4)) {
            char *colon=strchr(line,':');if(!colon || kind++)return diagnostic(t,"invalid_kind",0);
            char *v=trim(colon+1);
            if(!strcmp(v,"DIR"))item->directory=1;
            else if(strcmp(v,"FILE"))return diagnostic(t,"invalid_kind",0);
        } else if(!strncmp(line,"file length = ",14)) {
            if(size++ || number(line+14,&item->size))return diagnostic(t,"invalid_size",0);
        } else if(!strncmp(line,"modification time = ",20)) {
            if(stamp++)return diagnostic(t,"invalid_stamp",0);
            unsigned n=0;
            for(char *v=line+20;*v;++v) {
                if(*v==' ')continue;
                if(((*v<'0'||*v>'9') && *v!=':') || n>=sizeof(item->time)-1)return diagnostic(t,"invalid_stamp",0);
                item->time[n++]=*v;
            }
            if(!n)return diagnostic(t,"invalid_stamp",0);
        }
    }
    if(!item->name[0] || kind!=1 || size!=1 || stamp!=1)return diagnostic(t,"incomplete_metadata",0);
    t->metadata_retryable=0;
    return 1;
}
static int dcf_directory(const char *s) {
    if(strlen(s)!=8 || s[0]<'1'||s[0]>'9')return 0;
    for(unsigned i=1;i<3;++i)if(s[i]<'0'||s[i]>'9')return 0;
    return 1;
}
static int jpeg_name(const char *s) {return strlen(s)==12 && !strcmp(s+8,".JPG");}
static int scan_one_dir(struct transport *t,const char *directory,struct catalogue *out) {
    struct item item;int first=1,result;
    while((result=native_item(t,directory,first,&item))==1) {
        first=0;
        if(!item.directory && jpeg_name(item.name)) {
            if(out->count==MAX_FILES)return diagnostic(t,"file_capacity",0);
            struct entry *e=&out->items[out->count++];
            /* Directory is ms:/DCIM/ plus eight validated DCF characters. */
            snprintf(e->path,sizeof(e->path),"%.17s/%.12s",directory,item.name);
            strcpy(e->time,item.time);e->size=item.size;
            for(unsigned i=0;i+1<out->count;++i)if(!strcmp(out->items[i].path,e->path))return diagnostic(t,"duplicate_path",0);
        }
    }
    if(result<0){out->count=0;return result;}
    return 0;
}
static int inventory_dirs_with_hint(struct transport *t,struct catalogue *out,int latest_only,const char *hint_dir) {
    char directories[MAX_DIRS][48];unsigned count=0;int first=1,result;
    struct item item;out->count=0;t->scan_started=now_ms();
    if(latest_only && hint_dir && hint_dir[0])
        return scan_one_dir(t,hint_dir,out);
    if(latest_only)directories[0][0]=0;
    while((result=native_item(t,"ms:/DCIM",first,&item))==1) {
        first=0;
        if(item.directory && dcf_directory(item.name)) {
            char dir[48];
            /* dcf_directory already requires exactly eight characters. */
            snprintf(dir,sizeof(dir),"ms:/DCIM/%.8s",item.name);
            if(latest_only) {
                if(!count || strcmp(dir,directories[0])>0) {
                    strcpy(directories[0],dir);
                    count=1u;
                }
            } else {
                if(count==MAX_DIRS)return diagnostic(t,"directory_capacity",0);
                strcpy(directories[count++],dir);
            }
        }
    }
    if(result<0){out->count=0;return result;}
    for(unsigned d=0;d<count;++d) {
        result=scan_one_dir(t,directories[d],out);
        if(result<0)return result;
    }
    return 0;
}
static int inventory_dirs(struct transport *t,struct catalogue *out,int latest_only) {
    return inventory_dirs_with_hint(t,out,latest_only,NULL);
}
static int inventory(struct transport *t,struct catalogue *out) {
    return inventory_dirs(t,out,0);
}
int main(int argc,char **argv) {
    struct transport t={.pid=-1,.in=-1,.out=-1};int result=1,lock=-1;
    const char *reason="error";unsigned scans=0;int owner_stop=0;
    unsigned media_waits=0,media_polls=0,media_resumes=0;
    int media_unavailable=0;uint32_t media_since=0;
    uint32_t run_started=now_ms();
    int watch=argc==2 && !strcmp(argv[1],"watch");
    if(argc!=2 || (!watch && strcmp(argv[1],"scan")))return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    if(!monitor_matches()){printf("CAPTURE_FAIL monitor_pin\n");return 3;}
    if(watch && stop_requested()!=0){printf("CAPTURE_FAIL stale_stop\n");return 5;}
    lock=open(CAPTURE_LOCK,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(lock<0){printf("CAPTURE_FAIL lock\n");return 4;}
    close(lock);
    signal(SIGHUP,SIG_IGN);signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGPIPE,SIG_IGN);
    printf("CAPTURE_READY mode=%s read_only=1 close_notification_proven=0 revision=4 uptime_ms=%u active_ms=%u\n",
           argv[1],(unsigned)run_started,CAPTURE_ACTIVE_MS);
    if(transport_start(&t)){printf("CAPTURE_FAIL monitor_start\n");goto done;}
    struct catalogue current;static struct seen seen[MAX_FILES];unsigned seen_count=0;
    int baseline=inventory(&t,&current);
    if(baseline){
        if(baseline==-2)diagnostic(&t,"baseline_media_unavailable",0);
        printf("CAPTURE_FAIL baseline\n");goto done;
    }
    for(unsigned i=0;i<current.count;++i) {
        seen[i].value=current.items[i];seen[i].reported=1;
        printf("CAPTURE_EXISTING path=%s size=%u\n",current.items[i].path,current.items[i].size);
    }
    seen_count=current.count;
    printf("CAPTURE_BASELINE count=%u uptime_ms=%u\n",seen_count,(unsigned)now_ms());
    if(!watch){result=0;reason="scan_complete";goto done;}
    printf("CAPTURE_WAIT_UNPLUG ms=%u uptime_ms=%u\n",CAPTURE_WAIT_MS,(unsigned)now_ms());
    pause_ms(CAPTURE_WAIT_MS);
    uint32_t start=now_ms();
    printf("CAPTURE_ACTIVE uptime_ms=%u active_ms=%u\n",(unsigned)start,CAPTURE_ACTIVE_MS);
    while(!stopping && (uint32_t)(now_ms()-start)<CAPTURE_ACTIVE_MS) {
        int stop=stop_requested();
        if(stop<0){printf("CAPTURE_FAIL stop_marker\n");goto done;}
        if(stop){owner_stop=1;break;}
        int snapshot=inventory(&t,&current);
        if(snapshot==-2) {
            /* A complete native reply explicitly reports unmounted media.
             * Retry only a fresh enumeration at the existing bounded interval;
             * do not restart the monitor, reuse partial data or extend expiry. */
            if(!media_unavailable) {
                media_unavailable=1;media_since=now_ms();++media_waits;
                diagnostic(&t,"media_unavailable",0);
                printf("CAPTURE_MEDIA_WAIT event=%u uptime_ms=%u\n",media_waits,(unsigned)media_since);
                for(unsigned i=0;i<seen_count;++i)if(!seen[i].reported)seen[i].stable=0;
            }
            ++media_polls;pause_ms(CAPTURE_INTERVAL_MS);continue;
        }
        if(snapshot){printf("CAPTURE_FAIL snapshot\n");goto done;}
        if(media_unavailable) {
            media_unavailable=0;++media_resumes;
            printf("CAPTURE_MEDIA_RESUME event=%u wait_ms=%u uptime_ms=%u\n",
                   media_resumes,(unsigned)(now_ms()-media_since),(unsigned)now_ms());
        }
        uint32_t now=now_ms();
        ++scans;
        printf("CAPTURE_SNAPSHOT seq=%u uptime_ms=%u elapsed_ms=%u count=%u\n",
               scans,(unsigned)now,(unsigned)(now-start),current.count);
        for(unsigned i=0;i<seen_count;++i)seen[i].present=0;
        for(unsigned i=0;i<current.count;++i) {
            struct entry *e=&current.items[i];unsigned j=0;
            while(j<seen_count && strcmp(seen[j].value.path,e->path))++j;
            if(j==seen_count) {
                if(seen_count==MAX_FILES){printf("CAPTURE_FAIL seen_capacity\n");goto done;}
                seen[j].value=*e;seen[j].stable=now;++seen_count;
            }
            struct seen *s=&seen[j];
            if(!s->present && (!s->stable || s->value.size!=e->size || strcmp(s->value.time,e->time))) {
                s->value=*e;s->stable=now;
            }
            s->present=1;
            if(!s->reported && e->size && (uint32_t)(now-s->stable)>=CAPTURE_STABLE_MS) {
                s->reported=1;++candidates;
                printf("CAPTURE_CANDIDATE path=%s size=%u stable_ms=%u heuristic_only=1 uptime_ms=%u elapsed_ms=%u\n",
                       e->path,e->size,(unsigned)(now-s->stable),(unsigned)now,(unsigned)(now-start));
            }
        }
        for(unsigned i=0;i<seen_count;++i)if(!seen[i].present)seen[i].stable=0;
        pause_ms(CAPTURE_INTERVAL_MS);
    }
    result=stopping?20:(media_unavailable?21:0);
    reason=stopping?"signal":(owner_stop?"stop_file":
           (media_unavailable?"media_unavailable_expired":"expired"));
done:
    transport_stop(&t);unlink(CAPTURE_LOCK);
    printf("CAPTURE_STOP result=%d candidates=%u signal=%d reason=%s scans=%u uptime_ms=%u elapsed_ms=%u media_waits=%u media_polls=%u media_resumes=%u media_unavailable=%d\n",
           result,candidates,(int)stopping,reason,scans,(unsigned)now_ms(),(unsigned)(now_ms()-run_started),
           media_waits,media_polls,media_resumes,media_unavailable);
    return result;
}
