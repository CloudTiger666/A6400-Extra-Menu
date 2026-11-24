/* Serialize completed MPR inventories and menu-gated RAM/SD jobs. Stable
 * metadata is only a candidate heuristic; the pinned job verifies full JPEGs.
 * No direct Sony calls, UI modification, ptrace, or Linux SD mount is used here.
 */
#define _GNU_SOURCE
#include "film_capture_daemon_build.h"
#include "film_settings_maps.h"
#include "film_preview_fmt.h"
#include "film_album_index.h"
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/mount.h>
#endif
#include "film_runtime_log.h"
static struct film_log capture_log;
#define printf(...) film_log_printf(&capture_log,__VA_ARGS__)
#define puts(s) film_log_puts(&capture_log,(s))
#define putchar(c) film_log_putchar(&capture_log,(c))
#define main legacy_probe_main
#include "native_capture_probe.c"
#undef main

#ifndef FILM_CAPTURE_ACTIVE_MS
#define FILM_CAPTURE_ACTIVE_MS 600000u
#endif
#ifndef FILM_CAPTURE_PERSISTENT
#define FILM_CAPTURE_PERSISTENT 0
#endif
#ifndef FILM_CAPTURE_CLI_TIMEOUT_MS
#define FILM_CAPTURE_CLI_TIMEOUT_MS 2000u
#endif
#ifndef FILM_CAPTURE_JOB_TIMEOUT_MS
#define FILM_CAPTURE_JOB_TIMEOUT_MS 600000u
#endif
#ifndef FILM_CAPTURE_BASELINE_TIMEOUT_MS
#define FILM_CAPTURE_BASELINE_TIMEOUT_MS 60000u
#endif
#ifndef FILM_CAPTURE_LATEST_DIR_ONLY
#define FILM_CAPTURE_LATEST_DIR_ONLY 0
#endif
#ifndef FILM_CAPTURE_PROC_TRIGGER
#define FILM_CAPTURE_PROC_TRIGGER 0
#endif
#ifndef FILM_CAPTURE_IDLE_HEARTBEAT_MS
#define FILM_CAPTURE_IDLE_HEARTBEAT_MS 8000u
#endif
#ifndef FILM_CAPTURE_PROC_TICK_MS
#define FILM_CAPTURE_PROC_TICK_MS 200u
#endif
#ifndef FILM_CAPTURE_PROC_QUIET_TICKS
#define FILM_CAPTURE_PROC_QUIET_TICKS 4u
#endif
#ifndef FILM_CAPTURE_PROC_DELTA_MIN
#define FILM_CAPTURE_PROC_DELTA_MIN 2u
#endif
#ifndef FILM_CAPTURE_PROC_INTERRUPTS
#define FILM_CAPTURE_PROC_INTERRUPTS "/proc/interrupts"
#endif
#ifndef CAPTURE_BASELINE_INTERVAL_MS
#define CAPTURE_BASELINE_INTERVAL_MS CAPTURE_INTERVAL_MS
#endif
#ifndef FILM_CAPTURE_NATIVE_COMMAND
#define FILM_CAPTURE_NATIVE_COMMAND "/tmp_bt/a6400-film-native-command.elf"
#endif
#ifndef FILM_CAPTURE_CHARO_PCM
#define FILM_CAPTURE_CHARO_PCM "/tmp_bt/a6400-charo-shutter.pcm"
#endif
#ifndef FILM_CAPTURE_CUSTOM_CHARO_PCM
#define FILM_CAPTURE_CUSTOM_CHARO_PCM "/tmp_bt/a6400-custom-charo.pcm"
#endif
#ifndef FILM_CAPTURE_CUSTOM_CHECKED
#define FILM_CAPTURE_CUSTOM_CHECKED "/tmp_bt/a6400-custom-charo.checked"
#endif
#ifndef FILM_CAPTURE_CUSTOM_ACTIVE
#define FILM_CAPTURE_CUSTOM_ACTIVE "/tmp_bt/a6400-custom-charo.active"
#endif
#ifndef FILM_CAPTURE_DISABLE_CHARO
#define FILM_CAPTURE_DISABLE_CHARO "/tmp/disable_charo"
#endif
#ifndef FILM_CAPTURE_PREVIEW_BIN
#define FILM_CAPTURE_PREVIEW_BIN "/tmp_bt/a6400-film-preview.elf"
#endif
#define FILM_CHARO_PCM_BYTES 29760u
#define JOB_LOG_BYTES 65536u
#define METADATA_FAILURE_LIMIT 3u

enum state { OLD, WAITING, PENDING, APPLIED, OFF, FAILED, INFLIGHT };
struct queued {
    struct entry entry;
    uint32_t stable;
    enum state state;
    int present;
    char target[32];
};
static struct queued queue[MAX_FILES];
static unsigned queue_count,applied_jobs,off_jobs,failed_jobs,next_output;
static volatile sig_atomic_t daemon_signal;
static int own_manifest,own_log,own_inflight,retain_lock;
static pid_t job_pid;
static const char *phase="startup";
static char last_job_fail_hint[48];
static int capture_log_bad(void) {
    return capture_log.io_error || capture_log.bad_record || !capture_log.trusted;
}
static char job_log[JOB_LOG_BYTES+1u];

/* Do not set the legacy scanner's stopping flag. A stop must wait until its
 * current complete inventory, or its native job, has actually returned. */
static void daemon_stop(int sig) {daemon_signal=sig;}
static int absent_file(const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd>=0){close(fd);return 0;}
    return errno==ENOENT;
}
static int pin(const char *path,unsigned bytes,uint32_t expected) {
    unsigned char block[1024];unsigned total=0;uint32_t hash=2166136261u;
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_NOFOLLOW);if(fd<0)return 0;
    ssize_t n;
    while((n=read(fd,block,sizeof(block)))>0) {
        if(total>bytes || (unsigned)n>bytes-total){close(fd);return 0;}
        for(ssize_t i=0;i<n;++i)hash=(hash^block[i])*16777619u;
        total+=(unsigned)n;
    }
    close(fd);return !n && total==bytes && hash==expected;
}
static int pins(void) {
    return monitor_matches() &&
        pin(FILM_CAPTURE_SETTINGS,FILM_CAPTURE_SETTINGS_BYTES,FILM_CAPTURE_SETTINGS_FNV) &&
        pin(FILM_CAPTURE_SCRIPT,FILM_CAPTURE_SCRIPT_BYTES,FILM_CAPTURE_SCRIPT_FNV);
}
static const char *state_name(enum state value) {
    static const char *names[]={"baseline","waiting","pending","applied","off","failed","inflight"};
    return names[value];
}
static int source_path(const struct entry *entry,char *out,unsigned bytes) {
    const char *s=entry->path;
    if(strlen(s)!=30u || memcmp(s,"ms:/DCIM/",9) || s[17]!='/' || strcmp(s+26,".JPG"))return -1;
    for(unsigned i=9;i<26u;++i) {
        if(i==17u)continue;
        char c=s[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return -1;
    }
    int n=snprintf(out,bytes,"A:%s",s+3);
    return n>0 && (unsigned)n<bytes?0:-1;
}
static int save_manifest(void) {
    int flags=O_WRONLY|O_CREAT|O_NOFOLLOW|(own_manifest?O_TRUNC:O_EXCL);
    int fd=open(FILM_CAPTURE_MANIFEST,flags,0600);if(fd<0)return -1;
    own_manifest=1;FILE *file=fdopen(fd,"w");if(!file){close(fd);return -1;}
    fprintf(file,"FILM_CAPTURE_MANIFEST version=1\npid=%u\nphase=%s\njob_pid=%u\nuncertain=%d\ncount=%u\n",
            (unsigned)getpid(),phase,(unsigned)job_pid,retain_lock,queue_count);
    for(unsigned i=0;i<queue_count;++i) {
        struct queued *q=&queue[i];char source[40];
        if(source_path(&q->entry,source,sizeof(source))){fclose(file);return -1;}
        fprintf(file,"item state=%s source=%s bytes=%u stamp=%s target=%s stable_ms=%u present=%d\n",
                state_name(q->state),source,q->entry.size,q->entry.time,
                q->target[0]?q->target:"none",(unsigned)q->stable,q->present);
    }
    int error=ferror(file);return fclose(file) || error?-1:0;
}
static int save_inflight(const struct queued *q,const char *settings) {
    int flags=O_WRONLY|O_CREAT|O_NOFOLLOW|(own_inflight?O_TRUNC:O_EXCL);
    int fd=open(FILM_CAPTURE_INFLIGHT,flags,0600);if(fd<0)return -1;
    own_inflight=1;FILE *file=fdopen(fd,"w");if(!file){close(fd);return -1;}
    char source[40];if(source_path(&q->entry,source,sizeof(source))){fclose(file);return -1;}
    fprintf(file,"FILM_CAPTURE_INFLIGHT version=1\ndaemon_pid=%u\njob_pid=%u\nsource=%s\nbytes=%u\ntarget=%s\nsettings=%s\n",
            (unsigned)getpid(),(unsigned)job_pid,source,q->entry.size,q->target,settings);
    int error=ferror(file);return fclose(file) || error?-1:0;
}
static void reap_readonly(pid_t pid) {
    int status;kill(pid,SIGTERM);
    for(unsigned i=0;i<50u;++i) {
        pid_t got=waitpid(pid,&status,WNOHANG);
        if(got==pid || (got<0 && errno==ECHILD))return;
        pause_ms(10);
    }
    kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0 && errno==EINTR){}
}
/* Preserve bounded child evidence without allowing terminal controls or a
 * malformed CLI response to become a daemon log record. This never opens the
 * gate: unavailable settings still remain pending for a later fresh read. */
static int settings_unavailable(const char *reason,int error,pid_t pid,int status,
                                int done,uint32_t began,const char *out,unsigned used) {
    int source=0,at=0,has_source=0;
    if(used && sscanf(out,"FILM_UNAVAILABLE source=%d%n",&source,&at)==1 &&
       at>0 && (unsigned)at+1u==used && out[at]=='\n')has_source=1;
    const char *source_reason="none";
    if(has_source)switch(source) {
        case -17:source_reason="ui_artifact";break;
        case -16:source_reason="clock";break;
        case -15:source_reason="process_mem_read";break;
        case -14:source_reason="process_mem_open";break;
        case -13:source_reason="module_maps";break;
        case -12:source_reason="process_maps_read";break;
        case -11:source_reason="process_identity";break;
        case -10:source_reason="process_comm_read";break;
        case -1:source_reason="snapshot_or_job_invalid";break;
        case 1:source_reason="snapshot_inactive_or_stale";break;
        case 2:source_reason="snapshot_changed";break;
        case 10:source_reason="module_not_loaded";break;
        default:source_reason="unknown";break;
    }
    printf("FILM_CAPTURE_SETTINGS_DIAG reason=%s errno=%d uid=%u euid=%u child_pid=%u wait_done=%d exit=%d signal=%d elapsed_ms=%u stdout_bytes=%u has_source=%d cli_source=%d cli_reason=%s stdout_hex=",
           reason,error,(unsigned)getuid(),(unsigned)geteuid(),pid>0?(unsigned)pid:0u,
           done,done && WIFEXITED(status)?WEXITSTATUS(status):-1,
           done && WIFSIGNALED(status)?WTERMSIG(status):0,
           (unsigned)(now_ms()-began),used,has_source,source,source_reason);
    for(unsigned i=0;i<used;++i)printf("%02x",(unsigned)(unsigned char)out[i]);
    putchar('\n');return 11;
}
/* CLI is read-only, with exact bounded stdout and a separately pinned binary.
 * Its output carries enabled settings regardless of whether the menu is open. */
static int live_settings(char *out,unsigned capacity) {
    uint32_t began=now_ms();out[0]=0;errno=0;
    if(!pin(FILM_CAPTURE_SETTINGS,FILM_CAPTURE_SETTINGS_BYTES,FILM_CAPTURE_SETTINGS_FNV))
        return settings_unavailable("cli_pin",errno,-1,0,0,began,out,0);
    int pipefd[2];if(pipe(pipefd))return settings_unavailable("pipe",errno,-1,0,0,began,out,0);
    pid_t pid=fork();
    if(pid<0){int error=errno;close(pipefd[0]);close(pipefd[1]);
        return settings_unavailable("fork",error,-1,0,0,began,out,0);}
    if(!pid) {
        int null=open("/dev/null",O_WRONLY);
        if(null<0 || dup2(null,2)<0 || dup2(pipefd[1],1)<0)_exit(126);
        if(null!=2)close(null);
        close(pipefd[0]);close(pipefd[1]);
        execl(FILM_CAPTURE_SETTINGS,FILM_CAPTURE_SETTINGS,(char *)0);_exit(127);
    }
    close(pipefd[1]);
    if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)<0){int error=errno;close(pipefd[0]);reap_readonly(pid);
        return settings_unavailable("nonblocking",error,pid,0,0,began,out,0);}
    unsigned used=0;int status=0,done=0,eof=0,error=0;const char *failure=0;uint32_t start=now_ms();
    while(!(done && eof)) {
        if((uint32_t)(now_ms()-start)>=FILM_CAPTURE_CLI_TIMEOUT_MS){failure="cli_timeout";break;}
        char block[128];ssize_t n=read(pipefd[0],block,sizeof(block));
        if(n>0) {
            if(used+(unsigned)n>=capacity){
                unsigned keep=capacity-used-1u;
                memcpy(out+used,block,keep);used+=keep;failure="stdout_capacity";break;
            }
            memcpy(out+used,block,(unsigned)n);used+=(unsigned)n;
        } else if(!n)eof=1;
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){failure="stdout_read";error=errno;break;}
        if(!done) {
            pid_t got=waitpid(pid,&status,WNOHANG);
            if(got==pid)done=1;
            else if(got<0 && errno!=EINTR){failure="child_wait";error=errno;break;}
        }
        if(!(done && eof))pause_ms(5);
    }
    close(pipefd[0]);if(!done)reap_readonly(pid);
    out[used]=0;
    if(failure)return settings_unavailable(failure,error,pid,status,done,began,out,used);
    if(!WIFEXITED(status))return settings_unavailable("child_signal",0,pid,status,done,began,out,used);
    if(!used)return settings_unavailable("stdout_empty",0,pid,status,done,began,out,used);
    if(out[used-1]!='\n')return settings_unavailable("stdout_unterminated",0,pid,status,done,began,out,used);
    if(WEXITSTATUS(status)==10 && used==sizeof("FILM_OFF\n")-1u && !memcmp(out,"FILM_OFF\n",used))return 10;
    if(WEXITSTATUS(status))return settings_unavailable("cli_exit",0,pid,status,done,began,out,used);
    char preset[32];unsigned grain,halation;int at=0;
    if(sscanf(out,"FILM_JOB preset=%31[a-z0-9_] grain=%u halation=%u%n",preset,&grain,&halation,&at)!=3 ||
       at<0 || (unsigned)at+1u!=used || out[at]!='\n' || grain>3u || halation>1u)
        return settings_unavailable("job_format",0,pid,status,done,began,out,used);
    if(strcmp(preset,"portra_400") && strcmp(preset,"cinestill_800t") && strcmp(preset,"fuji_provia") &&
       strcmp(preset,"kodak_gold") && strcmp(preset,"tri_x_400") && strcmp(preset,"classic_chrome") &&
       strcmp(preset,"agfa_ultra_100") && strcmp(preset,"polaroid_669") && strcmp(preset,"fuji_acros") &&
       strcmp(preset,"polaroid_px680"))
        return settings_unavailable("job_preset",0,pid,status,done,began,out,used);
    char canonical[128];
    snprintf(canonical,sizeof(canonical),"FILM_JOB preset=%s grain=%u halation=%u\n",preset,grain,halation);
    if(strlen(canonical)!=used || memcmp(canonical,out,used))
        return settings_unavailable("job_noncanonical",0,pid,status,done,began,out,used);
    out[used-1u]=0;return 0;
}
static int line_starts_with(const char *line,size_t len,const char *prefix) {
    size_t pl=strlen(prefix);
    return len>=pl && !memcmp(line,prefix,pl);
}
static int line_has_token(const char *line,size_t len,const char *needle) {
    size_t nl=strlen(needle);
    if(nl>len)return 0;
    for(size_t i=0;i+nl<=len;++i)
        if(!memcmp(line+i,needle,nl))return 1;
    return 0;
}
/* Pre-write read ring_overflows that cleanly recover via RAM_JOB_READ_RETRY must
 * not halt the persistent daemon, while any unretried, post-target, or explicit
 * RAM_JOB_RETAINED marker fails closed. */
static int scan_job_retained(const char *log) {
    int read_uncertain=0;
    for(const char *p=log;*p;) {
        const char *nl=strchr(p,'\n');
        size_t len=nl?(size_t)(nl-p):strlen(p);
        if(len && p[len-1u]=='\r')--len;
        if(line_starts_with(p,len,"RAM_JOB_RETAINED "))return 1;
        if(read_uncertain) {
            if(line_starts_with(p,len,"RAM_JOB_READ_RETRY ")) {
                read_uncertain=0;
            } else if(!line_starts_with(p,len,"NATIVE_REPLY_TAIL_HEX ")) {
                return 1;
            }
        } else if(line_has_token(p,len,"uncertain=1")) {
            if(line_starts_with(p,len,"NATIVE_FAIL operation=read "))read_uncertain=1;
            else return 1;
        }
        if(!nl)break;
        p=nl+1;
    }
    return read_uncertain;
}
static int read_job_receipt(const struct queued *q,int *retained,int status) {
    int fd=open(FILM_CAPTURE_JOB_LOG,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
    unsigned used=0;ssize_t n;
    while((n=read(fd,job_log+used,JOB_LOG_BYTES-used))>0) {
        used+=(unsigned)n;
        if(used==JOB_LOG_BYTES){close(fd);return -1;}
    }
    close(fd);if(n<0)return -1;job_log[used]=0;
    *retained=scan_job_retained(job_log);
    last_job_fail_hint[0]=0;
#ifdef A6400_UI_LANG_SC
    if(strstr(job_log,"RAM_JOB_FAIL stage=scratch_capacity"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"原图超过15.9MB");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=settings_before_write-changed"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"转档中更改了菜单");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=memory_budget"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"内存不足");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=native_commit") ||
            strstr(job_log,"Can't open file A:/A6400/TEST"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"请建/A6400/TEST夹");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=render"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"图像解码失败");
#else
    if(strstr(job_log,"RAM_JOB_FAIL stage=scratch_capacity"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"原圖超過15.9MB");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=settings_before_write-changed"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"轉檔中更改了選單");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=memory_budget"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"記憶體不足");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=native_commit") ||
            strstr(job_log,"Can't open file A:/A6400/TEST"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"請建/A6400/TEST夾");
    else if(strstr(job_log,"RAM_JOB_FAIL stage=render"))
        snprintf(last_job_fail_hint,sizeof(last_job_fail_hint),"影像解碼失敗");
#endif
    /* Preserve a bounded diagnostic before the next job reuses its log. Hex
     * prevents child output from injecting records or terminal controls. */
    unsigned first=used>512u?used-512u:0u;char tail[1025];
    static const char hex[]="0123456789abcdef";
    for(unsigned i=first;i<used;++i) {
        unsigned char c=(unsigned char)job_log[i];
        tail[(i-first)*2u]=hex[c>>4];tail[(i-first)*2u+1u]=hex[c&15u];
    }
    tail[(used-first)*2u]=0;
    printf("FILM_CAPTURE_JOB_DIAG path=%s target=%s uptime_ms=%u exit=%d signal=%d retained=%d log_bytes=%u tail_hex=%s\n",
           q->entry.path,q->target,(unsigned)now_ms(),
           WIFEXITED(status)?WEXITSTATUS(status):-1,
           WIFSIGNALED(status)?WTERMSIG(status):0,*retained,used,tail);
    if(capture_log_bad())return -1;
    char source[40],prefix[160];if(source_path(&q->entry,source,sizeof(source)))return -1;
    snprintf(prefix,sizeof(prefix),"RAM_JOB_APPLIED source=%s target=%s ",source,q->target);
    unsigned reports=0;char *save=NULL;
    for(char *line=strtok_r(job_log,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        if(strstr(line,"RAM_JOB_APPLIED ")==line) {
            size_t size=strlen(line);
            static const char suffix[]=" source_access=read_only target_verified=1 original_verified=0";
            if(strstr(line,prefix)!=line || size<sizeof(suffix)-1u ||
               strcmp(line+size-(sizeof(suffix)-1u),suffix))return -1;
            ++reports;
        }
    }
    return reports==1u?1:reports? -1:0;
}
/* Start the script only after its PID and candidate have been recorded. The
 * child waits on a one-byte gate, so a failed receipt write cannot start I/O. */
#ifdef FILM_DAEMON_TOAST_RVA
static int open_ui_region(uint32_t rva,size_t bytes,int rw,uint32_t *out_addr) {
    DIR *dir=opendir("/proc");
    struct dirent *entry;
    pid_t im_pid=-1;
    if(dir) {
        while((entry=readdir(dir))) {
            char *end=0;long id=strtol(entry->d_name,&end,10);
            if(!*entry->d_name || *end || id<=0)continue;
            char p[64],comm[32];
            snprintf(p,sizeof(p),"/proc/%ld/comm",id);
            FILE *cf=fopen(p,"r");
            if(cf) {
                int ok=fgets(comm,sizeof(comm),cf) && (!strcmp(comm,"im.elf\n") || !strcmp(comm,"im.elf"));
                fclose(cf);
                if(ok){im_pid=(pid_t)id;break;}
            }
        }
        closedir(dir);
    }
    if(im_pid<=0)return -1;
    char maps_path[64],mem_path[64],line[512];
    snprintf(maps_path,sizeof(maps_path),"/proc/%ld/maps",(long)im_pid);
    snprintf(mem_path,sizeof(mem_path),"/proc/%ld/mem",(long)im_pid);
    FILE *mf=fopen(maps_path,"r");
    if(!mf)return -1;
    uint32_t target_addr=0;
    unsigned img_cnt=0, st_cnt=0;
    while(fgets(line,sizeof(line),mf)) {
        unsigned lo=0,hi=0,off=0;char perms[5]={0};int at=0;
        if(sscanf(line,"%x-%x %4s %x %*s %*s %n",&lo,&hi,perms,&off,&at)!=4 || at<=0 || lo>=hi)continue;
        size_t nl=strcspn(line+at,"\r\n");
        if(!off && !strcmp(perms,"r-xp") && strlen(FILM_DAEMON_UI_MODULE)==nl && !memcmp(line+at,FILM_DAEMON_UI_MODULE,nl)) {
            target_addr=lo+rva; ++img_cnt;
        }
        if(img_cnt==1 && !strcmp(perms,"rw-p") && target_addr>=lo && target_addr+bytes<=hi) {
            ++st_cnt;
        }
    }
    fclose(mf);
    if(img_cnt!=1 || st_cnt!=1)return -1;
    int fd=open(mem_path,rw?O_RDWR:O_RDONLY);
    if(fd<0)return -1;
    *out_addr=target_addr;
    return fd;
}
#endif
static void push_ui_toast(uint32_t kind,uint32_t duration_ms,const char *text) {
#ifdef FILM_DAEMON_TOAST_RVA
    if(!text)return;
    struct { uint32_t magic,seq,kind,duration_ms; char text[64]; } toast;
    uint32_t toast_addr=0;
    int fd=open_ui_region(FILM_DAEMON_TOAST_RVA,sizeof(toast),1,&toast_addr);
    if(fd<0)return;
    ssize_t pr=pread(fd,&toast,sizeof(toast),toast_addr);
    if(pr==sizeof(toast) && toast.magic==0x54534f54u) {
        toast.kind=kind;
        toast.duration_ms=duration_ms;
        memset(toast.text,0,sizeof(toast.text));
        snprintf(toast.text,sizeof(toast.text),"%s",text);
        toast.seq+=1u;
        const char *tp=(const char *)&toast;
        errno=0;
        ssize_t w1=pwrite(fd,tp+8u,sizeof(toast)-8u,toast_addr+8u);
        ssize_t w2=pwrite(fd,tp+4u,4u,toast_addr+4u);
        printf("FILM_TOAST_PUSH kind=%u seq=%u w1=%d w2=%d errno=%d\n",
               (unsigned)kind,(unsigned)toast.seq,(int)w1,(int)w2,errno);
    } else {
        printf("FILM_TOAST_SKIP addr=%08x pr=%d magic=%08x errno=%d\n",
               (unsigned)toast_addr,(int)pr,(unsigned)(pr==sizeof(toast)?toast.magic:0),errno);
    }
    close(fd);
#else
    (void)kind;(void)duration_ms;(void)text;
#endif
}
static void push_ui_status(unsigned active_jobs) {
#if defined(FILM_DAEMON_TOAST_RVA) && defined(FILM_DAEMON_STATUS_RVA)
    struct { uint32_t magic,seq,applied,scanned,max_files,custom_charo,active_jobs; } st;
    uint32_t addr=0;
    int fd=open_ui_region(FILM_DAEMON_STATUS_RVA,sizeof(st),1,&addr);
    if(fd<0)return;
    ssize_t pr=pread(fd,&st,sizeof(st),addr);
    if(pr==sizeof(st) && st.magic==0x53544156u) {
        st.applied=applied_jobs;
        st.scanned=queue_count;
        st.max_files=MAX_FILES;
        st.custom_charo=(access(FILM_CAPTURE_CUSTOM_ACTIVE,F_OK)==0)?1u:0u;
        st.active_jobs=active_jobs;
        st.seq+=1u;
        const char *sp=(const char *)&st;
        (void)pwrite(fd,sp+8u,sizeof(st)-8u,addr+8u);
        (void)pwrite(fd,sp+4u,4u,addr+4u);
    }
    close(fd);
#else
    (void)active_jobs;
#endif
}
static void toast_preset_name(const char *settings,char *out,size_t cap) {
    const char *p=strstr(settings,"preset=");
    if(!p){snprintf(out,cap,"film");return;}
    p+=7;
    size_t n=0;
    while(p[n] && p[n]!=' ' && p[n]!='\n' && n+1<cap){out[n]=p[n];++n;}
    out[n]=0;
}
static int read_ui_hires(void) {
#if defined(FILM_DAEMON_TOAST_RVA) && defined(FILM_DAEMON_HIRES_RVA)
    uint32_t addr=0;
    int fd=open_ui_region(FILM_DAEMON_HIRES_RVA,4u,0,&addr);
    if(fd<0)return 0;
    uint32_t val=0;ssize_t pr=pread(fd,&val,4,(off_t)addr);
    close(fd);
    return (pr==4 && val<=2u)?(int)val:0;
#else
    return 0;
#endif
}
#if defined(FILM_DAEMON_TOAST_RVA) && defined(FILM_DAEMON_V4_RVA)
static pid_t preview_pid;
static struct {
    int initialized;
    uint32_t latest_seq;
    uint32_t album_count;
    uint32_t album_head;
    uint32_t session_shots;
    int8_t shot_slots[A6400_SESSION_SHOTS_CAP];
    char slot_labels[A6400_ALBUM_SLOTS][48];
} v4_ring;

static void v4_ring_init(void) {
    if(v4_ring.initialized)return;
    memset(&v4_ring,0,sizeof(v4_ring));
    for(unsigned i=0;i<A6400_SESSION_SHOTS_CAP;++i)v4_ring.shot_slots[i]=-1;
    v4_ring.initialized=1;
}

static int copy_fd_exact(int src_fd,const char *dst_tmp,const char *dst_final,size_t bytes) {
    static unsigned char chunk[16384];
    int dfd=open(dst_tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(dfd<0)return -1;
    size_t left=bytes;
    while(left) {
        size_t want=left<sizeof(chunk)?left:sizeof(chunk);
        ssize_t r=read(src_fd,chunk,want);
        if(r<0 && errno==EINTR)continue;
        if(r<=0){close(dfd);unlink(dst_tmp);return -1;}
        size_t off=0;
        while(off<(size_t)r) {
            ssize_t w=write(dfd,chunk+off,(size_t)r-off);
            if(w<0 && errno==EINTR)continue;
            if(w<=0){close(dfd);unlink(dst_tmp);return -1;}
            off+=(size_t)w;
        }
        left-=(size_t)r;
    }
    if(close(dfd)!=0 || rename(dst_tmp,dst_final)!=0){unlink(dst_tmp);return -1;}
    return 0;
}

static struct {
    char names[A6400_ALBUM_SLOTS][A6400_ALBUM_NAME_BYTES];
    unsigned count;
} g_pv_lru;

static int pv_cache_path(const char *name,char *out,size_t cap) {
    if(!name || !film_album_name_ok(name))return -1;
    int n=snprintf(out,cap,"%s/c_%s.bin",A6400_PV_DIR,name);
    return (n>0 && (size_t)n<cap)?0:-1;
}

static int pv_lru_find(const char *name) {
    for(unsigned i=0;i<g_pv_lru.count;++i)
        if(!strcmp(g_pv_lru.names[i],name))return (int)i;
    return -1;
}

static void pv_lru_evict_for(const char *name) {
    if(!name || pv_lru_find(name)>=0)return;
    while(g_pv_lru.count>=A6400_ALBUM_SLOTS) {
        char old_path[160];
        if(pv_cache_path(g_pv_lru.names[0],old_path,sizeof(old_path))==0)
            unlink(old_path);
        for(unsigned i=1u;i<g_pv_lru.count;++i)
            memcpy(g_pv_lru.names[i-1u],g_pv_lru.names[i],A6400_ALBUM_NAME_BYTES);
        --g_pv_lru.count;
    }
}

static void pv_lru_touch(const char *name) {
    if(!name || !film_album_name_ok(name))return;
    int idx=pv_lru_find(name);
    if(idx>=0) {
        if((unsigned)idx+1u==g_pv_lru.count)return;
        char saved[A6400_ALBUM_NAME_BYTES];
        memcpy(saved,g_pv_lru.names[idx],A6400_ALBUM_NAME_BYTES);
        for(unsigned i=(unsigned)idx+1u;i<g_pv_lru.count;++i)
            memcpy(g_pv_lru.names[i-1u],g_pv_lru.names[i],A6400_ALBUM_NAME_BYTES);
        memcpy(g_pv_lru.names[g_pv_lru.count-1u],saved,A6400_ALBUM_NAME_BYTES);
        return;
    }
    pv_lru_evict_for(name);
    snprintf(g_pv_lru.names[g_pv_lru.count],A6400_ALBUM_NAME_BYTES,"%s",name);
    ++g_pv_lru.count;
}

static int pv_file_exact_size(const char *path,off_t want) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return 0;
    off_t sz=lseek(fd,0,SEEK_END);
    close(fd);
    return sz==want;
}

static int pv_cache_has(const char *name) {
    char cpath[160];
    if(pv_cache_path(name,cpath,sizeof(cpath))!=0)return 0;
    return pv_file_exact_size(cpath,(off_t)A6400_PV_ALBUM_FILE_BYTES);
}

static int pv_cache_activate(const char *name) {
    char cpath[160];
    if(pv_cache_path(name,cpath,sizeof(cpath))!=0)return -1;
    if(!pv_file_exact_size(cpath,(off_t)A6400_PV_ALBUM_FILE_BYTES))return -1;
    unlink(A6400_PV_CURRENT ".tmp");
    if(link(cpath,A6400_PV_CURRENT ".tmp")!=0)return -1;
    if(rename(A6400_PV_CURRENT ".tmp",A6400_PV_CURRENT)!=0) {
        unlink(A6400_PV_CURRENT ".tmp");
        return -1;
    }
    pv_lru_touch(name);
    return 0;
}

static void push_v4_ring_to_ui(int bump_latest_seq) {
    uint32_t base=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),1,&base);
    if(fd<0)return;
    uint32_t hdr[2]={0,0};
    if(pread(fd,hdr,sizeof(hdr),(off_t)base)==(ssize_t)sizeof(hdr) &&
       hdr[0]==A6400_V4_MAGIC && hdr[1]==A6400_V4_VERSION) {
        struct a6400_ui_v4_v1 img;
        memset(&img,0,sizeof(img));
        img.latest_seq=v4_ring.latest_seq;
        img.album_head=v4_ring.album_head;
        img.session_shots=v4_ring.session_shots;
        memcpy(img.shot_slots,v4_ring.shot_slots,sizeof(img.shot_slots));
        memcpy(img.slot_labels,v4_ring.slot_labels,sizeof(img.slot_labels));
        /* album_count is the TEST index. Stop before dexp_mode so a ring publish
         * cannot clear Double Exposure or the album request fields. */
        size_t tail_off=offsetof(struct a6400_ui_v4_v1,album_head);
        size_t tail_end=offsetof(struct a6400_ui_v4_v1,dexp_mode);
        (void)pwrite(fd,((const unsigned char *)&img)+tail_off,tail_end-tail_off,(off_t)(base+tail_off));
        if(bump_latest_seq) {
            size_t seq_off=offsetof(struct a6400_ui_v4_v1,latest_seq);
            (void)pwrite(fd,&img.latest_seq,4u,(off_t)(base+seq_off));
        }
    }
    close(fd);
}

static uint32_t read_ui_dexp(uint32_t *out_seq,uint32_t *out_pending) {
    if(out_seq)*out_seq=0u;
    if(out_pending)*out_pending=0u;
    uint32_t base=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),0,&base);
    if(fd<0)return 0u;
    uint32_t hdr[2]={0,0},dexp[3]={0,0,0};
    size_t doff=offsetof(struct a6400_ui_v4_v1,dexp_mode);
    if(pread(fd,hdr,sizeof(hdr),(off_t)base)==(ssize_t)sizeof(hdr) &&
       hdr[0]==A6400_V4_MAGIC && hdr[1]==A6400_V4_VERSION &&
       pread(fd,dexp,sizeof(dexp),(off_t)(base+doff))==(ssize_t)sizeof(dexp)) {
        close(fd);
        if(out_seq)*out_seq=dexp[1];
        if(out_pending)*out_pending=dexp[2];
        return dexp[0]<=A6400_DEXP_AVERAGE?dexp[0]:0u;
    }
    close(fd);
    return 0u;
}

static void push_v4_dexp_pending(uint32_t pending_val) {
    uint32_t base=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),1,&base);
    if(fd<0)return;
    uint32_t hdr[2]={0,0};
    size_t poff=offsetof(struct a6400_ui_v4_v1,dexp_pending);
    if(pread(fd,hdr,sizeof(hdr),(off_t)base)==(ssize_t)sizeof(hdr) &&
       hdr[0]==A6400_V4_MAGIC && hdr[1]==A6400_V4_VERSION) {
        (void)pwrite(fd,&pending_val,4u,(off_t)(base+poff));
    }
    close(fd);
}

static void start_preview_subsystem(void) {
    v4_ring_init();
#ifdef __linux__
    if(mkdir(A6400_PV_DIR,0700)==0 || errno==EEXIST) {
        int mounted=0;
        FILE *mf=fopen("/proc/mounts","r");
        if(mf) {
            char line[256];
            while(fgets(line,sizeof(line),mf)) {
                if(strstr(line," " A6400_PV_DIR " tmpfs ")){mounted=1;break;}
            }
            fclose(mf);
        }
        if(!mounted) {
            if(mount("a6400-pv",A6400_PV_DIR,"tmpfs",
                     MS_NOSUID|MS_NODEV|MS_NOEXEC,"size=10485760,nr_inodes=64,mode=700")==0) {
                printf("FILM_PREVIEW_TMPFS_READY dir=%s bytes=10485760\n",A6400_PV_DIR);
            } else {
                printf("FILM_PREVIEW_TMPFS_SKIP errno=%d\n",errno);
            }
        }
    }
#endif
    if(access(FILM_CAPTURE_PREVIEW_BIN,X_OK)!=0)return;
    pid_t me=getpid();
    pid_t pid=fork();
    if(pid==0) {
        int null=open("/dev/null",O_RDWR);
        if(null>=0){dup2(null,0);dup2(null,1);dup2(null,2);if(null>2)close(null);}
        char pbuf[24];
        snprintf(pbuf,sizeof(pbuf),"%ld",(long)me);
        execl(FILM_CAPTURE_PREVIEW_BIN,FILM_CAPTURE_PREVIEW_BIN,"watch",pbuf,(char *)0);
        _exit(127);
    }
    if(pid>0) {
        preview_pid=pid;
        printf("FILM_PREVIEW_HELPER_STARTED pid=%u\n",(unsigned)pid);
    }
}

static void stop_preview_subsystem(void) {
    if(preview_pid>0) {
        reap_readonly(preview_pid);
        preview_pid=0;
    }
}

static void publish_v4_shot(int applied,const char *settings,const char *target,int hires) {
    v4_ring_init();
    if(!applied) {
        v4_ring.shot_slots[v4_ring.session_shots%A6400_SESSION_SHOTS_CAP]=-1;
        v4_ring.session_shots+=1u;
        push_v4_ring_to_ui(0);
        return;
    }
    int sfd=open(A6400_PV_STAGED,O_RDONLY|O_NOFOLLOW);
    if(sfd<0) {
        v4_ring.shot_slots[v4_ring.session_shots%A6400_SESSION_SHOTS_CAP]=-1;
        v4_ring.session_shots+=1u;
        push_v4_ring_to_ui(0);
        return;
    }
    unsigned slot=(v4_ring.album_count==0u)?0u:((v4_ring.album_head+1u)%A6400_ALBUM_SLOTS);
    const char *base=target?strrchr(target,'/'):NULL;
    base=base?base+1:(target?target:"");
    int ok=(copy_fd_exact(sfd,A6400_PV_POPUP ".tmp",A6400_PV_POPUP,A6400_PV_POPUP_FILE_BYTES)==0);
    if(ok) {
        char ctmp[176],cdst[160];
        if(pv_cache_path(base,cdst,sizeof(cdst))==0) {
            pv_lru_evict_for(base);
            snprintf(ctmp,sizeof(ctmp),"%s.tmp",cdst);
            if(copy_fd_exact(sfd,ctmp,cdst,A6400_PV_ALBUM_FILE_BYTES)==0)
                pv_lru_touch(base);
        }
    }
    close(sfd);
    unlink(A6400_PV_STAGED);
    if(!ok) {
        v4_ring.shot_slots[v4_ring.session_shots%A6400_SESSION_SHOTS_CAP]=-1;
        v4_ring.session_shots+=1u;
        push_v4_ring_to_ui(0);
        return;
    }
    char preset[24];
    toast_preset_name(settings?settings:"",preset,sizeof(preset));
    const char *qstr=(hires==2)?"12MP":(hires==1?"6MP":"1.7MP");
    snprintf(v4_ring.slot_labels[slot],sizeof(v4_ring.slot_labels[slot]),
             "%.18s %s (%.14s)",preset,qstr,base);
    v4_ring.shot_slots[v4_ring.session_shots%A6400_SESSION_SHOTS_CAP]=(int8_t)slot;
    v4_ring.session_shots+=1u;
    v4_ring.album_head=slot;
    if(v4_ring.album_count<A6400_ALBUM_SLOTS)v4_ring.album_count+=1u;
    v4_ring.latest_seq+=1u;
    push_v4_ring_to_ui(1);
    printf("FILM_PREVIEW_PUBLISHED seq=%u slot=%u count=%u shots=%u\n",
           (unsigned)v4_ring.latest_seq,slot,(unsigned)v4_ring.album_count,(unsigned)v4_ring.session_shots);
}
#else
static void start_preview_subsystem(void) {}
static void stop_preview_subsystem(void) {}
static void publish_v4_shot(int applied,const char *settings,const char *target,int hires) {
    (void)applied;(void)settings;(void)target;(void)hires;
}
static uint32_t read_ui_dexp(uint32_t *out_seq,uint32_t *out_pending) {
    if(out_seq)*out_seq=0u;
    if(out_pending)*out_pending=0u;
    return 0u;
}
static void push_v4_dexp_pending(uint32_t pending_val) { (void)pending_val; }
#endif
static unsigned count_active_queue(void) {
    unsigned n=0;
    for(unsigned i=0;i<queue_count;++i)
        if(queue[i].present && (queue[i].state==PENDING || queue[i].state==WAITING))++n;
    return n;
}
static void make_target_path(const struct entry *e,unsigned seq,char *out,size_t cap) {
    /* Map ms:/DCIM/10661119/DSC09530.JPG -> A:/A6400/TEST/F9530xxx.JPG */
    const char *slash=strrchr(e->path,'/');
    const char *stem=slash?slash+1:e->path;
    size_t slen=strcspn(stem,".");
    char d[5]="0000";
    if(slen>=4u) {
        for(unsigned i=0;i<4u;++i) {
            char c=stem[slen-4u+i];
            d[i]=(c>='0' && c<='9')?c:'0';
        }
    }
    snprintf(out,cap,"A:/A6400/TEST/F%s%03u.JPG",d,seq%1000u);
}
static void push_toast_status(uint32_t kind,uint32_t duration_ms,const char *settings,const char *target,
                              unsigned qpos,unsigned qtot,int hires) {
    char preset[24],qtag[16]="",msg[64];
    toast_preset_name(settings,preset,sizeof(preset));
    if(qtot>1u)snprintf(qtag,sizeof(qtag)," (%u/%u)",qpos%1000u,qtot%1000u);
#ifdef A6400_UI_LANG_SC
    if(kind==1u)snprintf(msg,sizeof(msg),"处理中%.12s: %.20s%s...",qtag,preset,
                         hires==2?" 12MP":(hires==1?" 6MP":""));
    else if(kind==2u) {
        const char *base=target?strrchr(target,'/'):NULL;
        base=base?base+1:(target?target:"");
        snprintf(msg,sizeof(msg),"OK%.12s: %.16s (%.16s)",qtot>1u?qtag:" 已存SD",preset,base);
    } else if(last_job_fail_hint[0]) {
        snprintf(msg,sizeof(msg),"处理失败: %.42s",last_job_fail_hint);
    } else snprintf(msg,sizeof(msg),"处理失败%.12s: %.20s",qtag,preset);
#else
    if(kind==1u)snprintf(msg,sizeof(msg),"處理中%.12s: %.20s%s...",qtag,preset,
                         hires==2?" 12MP":(hires==1?" 6MP":""));
    else if(kind==2u) {
        const char *base=target?strrchr(target,'/'):NULL;
        base=base?base+1:(target?target:"");
        snprintf(msg,sizeof(msg),"OK%.12s: %.16s (%.16s)",qtot>1u?qtag:" 已存SD",preset,base);
    } else if(last_job_fail_hint[0]) {
        snprintf(msg,sizeof(msg),"處理失敗: %.42s",last_job_fail_hint);
    } else snprintf(msg,sizeof(msg),"處理失敗%.12s: %.20s",qtag,preset);
#endif
    push_ui_toast(kind,duration_ms,msg);
}
static void update_latest_dir(char *latest_dir,size_t cap,const struct catalogue *c) {
    if(!c->count || strncmp(c->items[0].path,"ms:/DCIM/",9u) ||
       strlen(c->items[0].path)<18u || c->items[0].path[17]!='/')return;
    snprintf(latest_dir,cap,"%.17s",c->items[0].path);
}
static int run_job(struct queued *q,const char *settings,int hires) {
    if(capture_log_bad() || !pins() || !absent_file(FILM_CAPTURE_JOB_LOCK) || !absent_file(FILM_CAPTURE_INFLIGHT))return 21;
    int flags=O_WRONLY|O_CREAT|O_NOFOLLOW|(own_log?O_TRUNC:O_EXCL);
    int log=open(FILM_CAPTURE_JOB_LOG,flags,0600);if(log<0)return 21;
    own_log=1;
    q->state=INFLIGHT;phase="job_prepare";
    if(save_inflight(q,settings) || save_manifest()){close(log);return 21;}
    int gate[2];if(pipe(gate)){close(log);return 21;}
    job_pid=fork();
    if(job_pid<0){close(log);close(gate[0]);close(gate[1]);job_pid=0;return 21;}
    if(!job_pid) {
        close(gate[1]);char token;
        ssize_t n;do n=read(gate[0],&token,1);while(n<0 && errno==EINTR);
        close(gate[0]);if(n!=1 || token!='G')_exit(125);
        if(dup2(log,1)<0 || dup2(log,2)<0)_exit(126);
        close(log);char source[40],bytes[16];
        if(source_path(&q->entry,source,sizeof(source)))_exit(124);
        snprintf(bytes,sizeof(bytes),"%u",q->entry.size);
        const char *qarg=(hires==2)?"ultra12m":(hires==1?"hires":(char *)0);
        execl("/bin/sh","sh",FILM_CAPTURE_SCRIPT,"apply",source,bytes,q->target,qarg,(char *)0);_exit(127);
    }
    close(log);close(gate[0]);phase="job";
    if(save_inflight(q,settings) || save_manifest()) {
        close(gate[1]);reap_readonly(job_pid);job_pid=0;return 21;
    }
    char token='G';ssize_t released=write(gate[1],&token,1);close(gate[1]);
    if(released!=1){reap_readonly(job_pid);job_pid=0;return 21;}
    uint32_t start=now_ms();int status;
    for(;;) {
        pid_t got=waitpid(job_pid,&status,WNOHANG);
        if(got==job_pid)break;
        if(got<0 && errno!=EINTR)return 21;
        if((uint32_t)(now_ms()-start)>=FILM_CAPTURE_JOB_TIMEOUT_MS) {
            /* Never kill a possibly outstanding native operation. Leave the
             * child PID, manifest, inflight and locks for explicit recovery. */
            printf("FILM_CAPTURE_JOB_UNCONFIRMED reason=deadline pid=%u\n",(unsigned)job_pid);
            return 21;
        }
        pause_ms(10);
    }
    job_pid=0;
    int retained=0,receipt=read_job_receipt(q,&retained,status);
    if(!WIFEXITED(status) || WEXITSTATUS(status)==21 || retained ||
       !absent_file(FILM_CAPTURE_JOB_LOCK) || receipt<0)return 21;
    int rc=WEXITSTATUS(status);
    if(!rc && receipt!=1)return 21;
    if(rc && receipt)return 21;
    q->state=rc?FAILED:APPLIED;
    if(rc)++failed_jobs;else ++applied_jobs;
    phase="scanning";
    if(save_manifest() || unlink(FILM_CAPTURE_INFLIGHT))return 21;
    own_inflight=0;
    printf("FILM_CAPTURE_JOB_DONE result=%d path=%s target=%s applied=%u failed=%u uptime_ms=%u\n",
           rc,q->entry.path,q->target,applied_jobs,failed_jobs,(unsigned)now_ms());
    return 0;
}

static const char *dexp_arg_name(uint32_t mode) {
    if(mode==A6400_DEXP_LIGHTEN)return "lighten";
    if(mode==A6400_DEXP_AVERAGE)return "average";
    return "screen";
}

static const char *dexp_ui_name(uint32_t mode) {
#ifdef A6400_UI_LANG_SC
    if(mode==A6400_DEXP_LIGHTEN)return "变亮";
    if(mode==A6400_DEXP_AVERAGE)return "平均";
    return "滤色";
#else
    if(mode==A6400_DEXP_LIGHTEN)return "變亮";
    if(mode==A6400_DEXP_AVERAGE)return "平均";
    return "濾色";
#endif
}

static int run_dexp_job(const char *source1,unsigned bytes1,struct queued *q,uint32_t dmode,int hires) {
    if(capture_log_bad() || !pins() || !absent_file(FILM_CAPTURE_JOB_LOCK) || !absent_file(FILM_CAPTURE_INFLIGHT))return 21;
    int flags=O_WRONLY|O_CREAT|O_NOFOLLOW|(own_log?O_TRUNC:O_EXCL);
    int log=open(FILM_CAPTURE_JOB_LOG,flags,0600);if(log<0)return 21;
    own_log=1;
    q->state=INFLIGHT;phase="job_prepare";
    char dsettings[96];
    snprintf(dsettings,sizeof(dsettings),"FILM_DEXP mode=%s src1=%.40s",dexp_arg_name(dmode),source1);
    if(save_inflight(q,dsettings) || save_manifest()){close(log);return 21;}
    int gate[2];if(pipe(gate)){close(log);return 21;}
    job_pid=fork();
    if(job_pid<0){close(log);close(gate[0]);close(gate[1]);job_pid=0;return 21;}
    if(!job_pid) {
        close(gate[1]);char token;
        ssize_t n;do n=read(gate[0],&token,1);while(n<0 && errno==EINTR);
        close(gate[0]);if(n!=1 || token!='G')_exit(125);
        if(dup2(log,1)<0 || dup2(log,2)<0)_exit(126);
        close(log);char source2[40],b1[16],b2[16];
        if(source_path(&q->entry,source2,sizeof(source2)))_exit(124);
        snprintf(b1,sizeof(b1),"%u",bytes1);
        snprintf(b2,sizeof(b2),"%u",q->entry.size);
        const char *qarg=(hires>=1)?"hires":(char *)0;
        const char *marg=dexp_arg_name(dmode);
        execl("/bin/sh","sh",FILM_CAPTURE_SCRIPT,"dexp",source1,b1,source2,b2,q->target,marg,qarg,(char *)0);
        _exit(127);
    }
    close(log);close(gate[0]);phase="job";
    if(save_inflight(q,dsettings) || save_manifest()) {
        close(gate[1]);reap_readonly(job_pid);job_pid=0;return 21;
    }
    char token='G';ssize_t released=write(gate[1],&token,1);close(gate[1]);
    if(released!=1){reap_readonly(job_pid);job_pid=0;return 21;}
    uint32_t start=now_ms();int status;
    for(;;) {
        pid_t got=waitpid(job_pid,&status,WNOHANG);
        if(got==job_pid)break;
        if(got<0 && errno!=EINTR)return 21;
        if((uint32_t)(now_ms()-start)>=FILM_CAPTURE_JOB_TIMEOUT_MS) {
            printf("FILM_CAPTURE_JOB_UNCONFIRMED reason=deadline pid=%u\n",(unsigned)job_pid);
            return 21;
        }
        pause_ms(10);
    }
    job_pid=0;
    int retained=0,receipt=read_job_receipt(q,&retained,status);
    if(!WIFEXITED(status) || WEXITSTATUS(status)==21 || retained ||
       !absent_file(FILM_CAPTURE_JOB_LOCK) || receipt<0)return 21;
    int rc=WEXITSTATUS(status);
    if(!rc && receipt!=1)return 21;
    if(rc && receipt)return 21;
    q->state=rc?FAILED:APPLIED;
    if(rc)++failed_jobs;else ++applied_jobs;
    phase="scanning";
    if(save_manifest() || unlink(FILM_CAPTURE_INFLIGHT))return 21;
    own_inflight=0;
    printf("FILM_CAPTURE_DEXP_DONE result=%d src1=%s path=%s target=%s mode=%s applied=%u failed=%u uptime_ms=%u\n",
           rc,source1,q->entry.path,q->target,dexp_arg_name(dmode),applied_jobs,failed_jobs,(unsigned)now_ms());
    return 0;
}
static void compact_superseded_baseline(const struct catalogue *current) {
    if(!FILM_CAPTURE_LATEST_DIR_ONLY || !current->count)return;
    unsigned keep=0;
    for(unsigned i=0;i<queue_count;++i) {
        if(queue[i].state==OLD && strncmp(queue[i].entry.path,current->items[0].path,18u)<0)
            continue;
        if(keep!=i)queue[keep]=queue[i];
        ++keep;
    }
    queue_count=keep;
}
static int update_queue(const struct catalogue *current) {
    uint32_t now=now_ms();
    compact_superseded_baseline(current);
    for(unsigned i=0;i<queue_count;++i)queue[i].present=0;
    for(unsigned i=0;i<current->count;++i) {
        const struct entry *e=&current->items[i];char source[40];
        if(source_path(e,source,sizeof(source)))return -1;
        unsigned j=0;while(j<queue_count && strcmp(queue[j].entry.path,e->path))++j;
        if(j==queue_count) {
            if(queue_count==MAX_FILES)return -1;
            queue[j].entry=*e;queue[j].stable=now;queue[j].state=WAITING;++queue_count;
            printf("FILM_CAPTURE_WAITING source=%s bytes=%u uptime_ms=%u\n",source,e->size,(unsigned)now);
        }
        struct queued *q=&queue[j];q->present=1;
        if(q->state!=WAITING && q->state!=PENDING)continue;
        if(!q->stable || q->entry.size!=e->size || strcmp(q->entry.time,e->time)) {
            q->entry=*e;q->stable=now;q->state=WAITING;
        }
        if(e->size && (uint32_t)(now-q->stable)>=CAPTURE_STABLE_MS && q->state==WAITING) {
            q->state=PENDING;
            printf("FILM_CAPTURE_PENDING source=%s bytes=%u heuristic_only=1 uptime_ms=%u\n",source,e->size,(unsigned)now);
        }
    }
    for(unsigned i=0;i<queue_count;++i)if(!queue[i].present &&
        (queue[i].state==WAITING || queue[i].state==PENDING)) {
        queue[i].stable=0;queue[i].state=WAITING;
    }
    return 0;
}
static void reset_candidate_stability(void) {
    for(unsigned i=0;i<queue_count;++i)if(queue[i].state==WAITING || queue[i].state==PENDING) {
        queue[i].stable=0;queue[i].state=WAITING;
    }
}
static int any_waiting_candidate(void) {
    for(unsigned i=0;i<queue_count;++i)if(queue[i].state==WAITING)return 1;
    return 0;
}
static int should_break_wait(uint32_t active_start,int check_expiry) {
    if(daemon_signal || stop_requested()!=0)return 1;
    if(check_expiry && !FILM_CAPTURE_PERSISTENT &&
       (uint32_t)(now_ms()-active_start)>=FILM_CAPTURE_ACTIVE_MS)return 1;
    return 0;
}
static void pause_interruptible(uint32_t ms,uint32_t active_start,int check_expiry) {
    uint32_t began=now_ms();
    for(;;) {
        uint32_t elapsed=(uint32_t)(now_ms()-began);
        if(elapsed>=ms || should_break_wait(active_start,check_expiry))return;
        uint32_t rem=ms-elapsed;
        pause_ms(rem>200u?200u:rem);
    }
}
#if FILM_CAPTURE_PROC_TRIGGER
static int is_mmc_irq_line(const char *line) {
    for(const char *p=line;(p=strstr(p,"mmc0"))!=NULL;p+=4) {
        char prev=p==line?' ':p[-1],next=p[4];
        if((prev==' '||prev=='\t') && (!next||next==' '||next=='\t'||next=='\r'||next=='\n'||next==','))
            return 1;
    }
    while(*line==' '||*line=='\t')++line;
    return !strncmp(line,"218:",4u);
}
static void parse_mmc_irq_line(const char *line,uint64_t *total,unsigned *sources) {
    if(!is_mmc_irq_line(line))return;
    const char *p=strchr(line,':');
    if(!p)return;
    ++p;++*sources;
    for(unsigned cpu=0;cpu<8u;++cpu) {
        while(*p==' '||*p=='\t')++p;
        if(*p<'0'||*p>'9')break;
        uint64_t v=0;
        while(*p>='0'&&*p<='9')v=v*10u+(uint64_t)(*p++-'0');
        *total+=v;
    }
}
static int sd_io_sample(uint64_t *out) {
    int fd=open(FILM_CAPTURE_PROC_INTERRUPTS,O_RDONLY|O_NONBLOCK);
    if(fd<0)return 0;
    char buf[512],line[256];
    unsigned pos=0,sources=0;
    int skip_eol=0;
    uint64_t total=0;
    ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0) {
        for(ssize_t i=0;i<n;++i) {
            char c=buf[i];
            if(c=='\n') {
                if(!skip_eol){line[pos]=0;parse_mmc_irq_line(line,&total,&sources);}
                pos=0;skip_eol=0;
            } else if(!skip_eol) {
                if(pos+1u<sizeof(line))line[pos++]=c;
                else skip_eol=1;
            }
        }
    }
    if(pos && !skip_eol){line[pos]=0;parse_mmc_irq_line(line,&total,&sources);}
    close(fd);
    *out=total;
    return sources>0u;
}
#endif
static int album_flip_waiting(void);
static void wait_before_scan(uint32_t active_start,int *post_trigger_follow) {
    if(album_flip_waiting())return;
    int urgent=any_waiting_candidate() || *post_trigger_follow;
    *post_trigger_follow=0;
    if(urgent) {
        pause_interruptible(CAPTURE_INTERVAL_MS,active_start,1);
        return;
    }
#if FILM_CAPTURE_PROC_TRIGGER
    uint64_t last=0;
    if(sd_io_sample(&last)) {
        uint32_t began=now_ms();
        int sd_active=0;unsigned quiet=0;
        while((uint32_t)(now_ms()-began)<FILM_CAPTURE_IDLE_HEARTBEAT_MS) {
            if(should_break_wait(active_start,1) || album_flip_waiting())return;
            uint32_t rem=FILM_CAPTURE_IDLE_HEARTBEAT_MS-(uint32_t)(now_ms()-began);
            pause_ms(rem>FILM_CAPTURE_PROC_TICK_MS?FILM_CAPTURE_PROC_TICK_MS:rem);
            if(should_break_wait(active_start,1) || album_flip_waiting())return;
            uint64_t cur=0;
            if(!sd_io_sample(&cur))break;
            uint64_t delta=cur>=last?(cur-last):0;
            last=cur;
            if(delta>FILM_CAPTURE_PROC_DELTA_MIN){sd_active=1;quiet=0;}
            else if(sd_active && ++quiet>=FILM_CAPTURE_PROC_QUIET_TICKS) {
                *post_trigger_follow=1;
                return;
            }
        }
        return;
    }
#endif
    pause_interruptible(CAPTURE_INTERVAL_MS,active_start,1);
}
static int metadata_retry(unsigned *failures,const char *stage) {
    ++*failures;
    printf("FILM_CAPTURE_SCAN_%s stage=%s failures=%u limit=%u next=%s uptime_ms=%u\n",
           *failures<METADATA_FAILURE_LIMIT?"RETRY":"EXHAUSTED",stage,
           *failures,METADATA_FAILURE_LIMIT,
           *failures<METADATA_FAILURE_LIMIT?"root_ffirst":"stop",(unsigned)now_ms());
    return *failures<METADATA_FAILURE_LIMIT;
}
static void metadata_resumed(unsigned *failures,const char *stage) {
    if(*failures)printf("FILM_CAPTURE_SCAN_RESUMED stage=%s failures=%u uptime_ms=%u\n",
                        stage,*failures,(unsigned)now_ms());
    *failures=0;
}
static int run_sound_cmd(const char *sd_path) {
    pid_t pid=fork();
    if(pid<0)return -1;
    if(pid==0) {
        int devnull=open("/dev/null",O_RDWR);
        if(devnull>=0){dup2(devnull,0);dup2(devnull,1);dup2(devnull,2);if(devnull>2)close(devnull);}
        char *const argv[]={(char *)FILM_CAPTURE_NATIVE_COMMAND,(char *)"sound",(char *)sd_path,NULL};
        execv(FILM_CAPTURE_NATIVE_COMMAND,argv);
        _exit(127);
    }
    int status=0;uint32_t began=now_ms();
    for(;;) {
        pid_t got=waitpid(pid,&status,WNOHANG);
        if(got==pid)return WIFEXITED(status)?WEXITSTATUS(status):-1;
        if(got<0){if(errno==EINTR)continue;return -1;}
        if((uint32_t)(now_ms()-began)>=FILM_CAPTURE_CLI_TIMEOUT_MS) {
            kill(pid,SIGKILL);
            waitpid(pid,&status,0);
            return -1;
        }
        pause_ms(10);
    }
}
static void sync_custom_charo(struct transport *t) {
    if(access(FILM_CAPTURE_CHARO_PCM,F_OK)!=0 ||
       access(FILM_CAPTURE_DISABLE_CHARO,F_OK)==0 ||
       access(FILM_CAPTURE_CUSTOM_CHECKED,F_OK)==0 ||
       access(FILM_CAPTURE_NATIVE_COMMAND,X_OK)!=0)return;
    int cfd=open(FILM_CAPTURE_CUSTOM_CHECKED,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0644);
    if(cfd<0)return;
    close(cfd);
    /* Reap MPR before running mwf_monio IFSYS command. */
    transport_stop(t);
    static const char *const candidates[]={"A:/A6400/CHARO.PCM","A:/A6400/SHUTTER.PCM"};
    const char *loaded_path=NULL;
    for(unsigned i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i) {
        if(!absent_file(FILM_CAPTURE_CUSTOM_CHARO_PCM))break;
        int rc=run_sound_cmd(candidates[i]);
        if(rc==0){loaded_path=candidates[i];break;}
        if(rc!=14){printf("FILM_CUSTOM_CHARO_CMD_FAIL path=%s rc=%d\n",candidates[i],rc);break;}
    }
    if(!loaded_path){unlink(FILM_CAPTURE_CUSTOM_CHARO_PCM);return;}
    static unsigned char pcm[FILM_CHARO_PCM_BYTES];
    int sfd=open(FILM_CAPTURE_CUSTOM_CHARO_PCM,O_RDONLY|O_NOFOLLOW);
    if(sfd<0){unlink(FILM_CAPTURE_CUSTOM_CHARO_PCM);return;}
    unsigned total=0;ssize_t n;
    while(total<FILM_CHARO_PCM_BYTES && (n=read(sfd,pcm+total,FILM_CHARO_PCM_BYTES-total))>0)
        total+=(unsigned)n;
    unsigned char extra=0;
    ssize_t tail=n<0?-1:read(sfd,&extra,1);
    close(sfd);
    unlink(FILM_CAPTURE_CUSTOM_CHARO_PCM);
    if(n<0 || tail!=0 || total==0u) {
        printf("FILM_CUSTOM_CHARO_SKIP path=%s reason=invalid_size bytes=%u tail=%d\n",
               loaded_path,total,(int)tail);
        return;
    }
    if(total<FILM_CHARO_PCM_BYTES)memset(pcm+total,0,FILM_CHARO_PCM_BYTES-total);
    /* Write in-place (no unlink/O_TRUNC) so MS_BIND stays attached to same inode. */
    int dfd=open(FILM_CAPTURE_CHARO_PCM,O_WRONLY|O_NOFOLLOW);
    if(dfd<0){printf("FILM_CUSTOM_CHARO_OPEN_FAIL errno=%d\n",errno);return;}
    unsigned wrote=0;
    while(wrote<FILM_CHARO_PCM_BYTES) {
        ssize_t w=write(dfd,pcm+wrote,FILM_CHARO_PCM_BYTES-wrote);
        if(w<0 && errno==EINTR)continue;
        if(w<=0)break;
        wrote+=(unsigned)w;
    }
    close(dfd);
    if(wrote==FILM_CHARO_PCM_BYTES) {
        int afd=open(FILM_CAPTURE_CUSTOM_ACTIVE,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0644);
        if(afd>=0)close(afd);
        printf("FILM_CUSTOM_CHARO_OK source=%s in_bytes=%u out_bytes=%u\n",
               loaded_path,total,wrote);
    } else {
        printf("FILM_CUSTOM_CHARO_WRITE_FAIL wrote=%u\n",wrote);
    }
}

#if defined(FILM_DAEMON_TOAST_RVA) && defined(FILM_DAEMON_V4_RVA)
static struct film_album_index g_album;
static int g_album_scanned;
static uint32_t g_album_scan_done,g_album_served_seq,g_album_served_slot,g_album_fail_seq,g_album_fail_ms;
static unsigned g_album_fail_count;

static int read_album_request(uint32_t *mode,uint32_t *view_seq,uint32_t *view_slot,uint32_t *scan_req);
static int album_request_matches(uint32_t seq,uint32_t slot) {
    if(daemon_signal || stop_requested()!=0)return 0;
    uint32_t mode=0,view_seq=0,view_slot=0,scan_req=0;
    if(read_album_request(&mode,&view_seq,&view_slot,&scan_req)!=0)return 0;
    (void)mode;
    if(seq==0u)return view_seq==0u && scan_req==g_album_scan_done;
    return view_seq!=0 && view_seq==seq && view_slot==slot;
}
/* -2: the UI left this photo. The child is stopped and its RAM is released. */
static int run_exec_until_left(const char *file,char *const argv[],uint32_t seq,uint32_t slot) {
    pid_t pid=fork();
    if(pid<0)return -1;
    if(pid==0){(void)setpgid(0,0);execv(file,argv);_exit(127);}
    int status=0;
    for(;;) {
        pid_t got=waitpid(pid,&status,WNOHANG);
        if(got==pid)return status==0?0:-1;
        if(got<0 && errno!=EINTR) {
            kill(-pid,SIGKILL);
            kill(pid,SIGKILL);
            while(waitpid(pid,&status,0)<0 && errno==EINTR){}
            return -1;
        }
        pause_ms(20);
        if(!album_request_matches(seq,slot)){
            kill(-pid,SIGTERM);
            reap_readonly(pid);
            kill(-pid,SIGKILL);
            return -2;
        }
    }
}
static int inventory_test_album(struct transport *t,struct film_album_index *out) {
    film_album_index_reset(out);
    t->scan_started=now_ms();
    struct item item;
    int first=1,result;
    while((result=native_item(t,"ms:/A6400/TEST",first,&item))==1) {
        first=0;
        if(!item.directory)film_album_index_add(out,item.name);
    }
    return result<0?result:0;
}
static void publish_album_catalog(const struct film_album_index *idx) {
    uint32_t base=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),1,&base);
    if(fd<0)return;
    uint32_t count=idx->count,trunc=idx->truncated?1u:0u;
    (void)pwrite(fd,&count,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_count)));
    (void)pwrite(fd,&trunc,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_truncated)));
    close(fd);
}
static void publish_album_ready(uint32_t seq,uint32_t slot,uint32_t scan_done,const char *label) {
    uint32_t base=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),1,&base);
    if(fd<0)return;
    (void)pwrite(fd,&seq,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_ready_seq)));
    (void)pwrite(fd,&slot,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_ready_slot)));
    (void)pwrite(fd,&scan_done,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_scan_done)));
    if(label) {
        char text[48];
        memset(text,0,sizeof(text));
        snprintf(text,sizeof(text),"%s",label);
        (void)pwrite(fd,text,sizeof(text),(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_label)));
    }
    close(fd);
}
static int read_album_request(uint32_t *mode,uint32_t *view_seq,uint32_t *view_slot,uint32_t *scan_req) {
    uint32_t base=0,magic=0;
    int fd=open_ui_region(FILM_DAEMON_V4_RVA,sizeof(struct a6400_ui_v4_v1),0,&base);
    if(fd<0)return -1;
    int ok=pread(fd,&magic,4,(off_t)base)==4 && magic==A6400_V4_MAGIC &&
        pread(fd,mode,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,display_mode)))==4 &&
        pread(fd,view_seq,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_view_seq)))==4 &&
        pread(fd,view_slot,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_view_slot)))==4 &&
        pread(fd,scan_req,4,(off_t)(base+offsetof(struct a6400_ui_v4_v1,album_scan_req)))==4;
    close(fd);
    return ok?0:-1;
}
#define ALBUM_JOB_DIR "/tmp_bt/a6400-film-job"
/* /tmp_bt is a 1-2 MiB tmpfs; a graded JPEG does not fit there.
 * film_ram_job.sh refuses to start while this directory exists, so it is
 * mounted only for one decode and removed again. 0 = mounted here. */
static int album_input_mount(void) {
    FILE *mf=fopen("/proc/mounts","r");
    if(!mf)return -1;
    char line[256];int mounted=0;
    while(fgets(line,sizeof(line),mf))if(strstr(line," " ALBUM_JOB_DIR " ")){mounted=1;break;}
    fclose(mf);
    if(mounted)return -1;
    if(mkdir(ALBUM_JOB_DIR,0700) && errno!=EEXIST)return -1;
#ifdef __linux__
    if(!mount("a6400-album",ALBUM_JOB_DIR,"tmpfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,
              "size=33554432,nr_inodes=16,mode=700"))return 0;
#endif
    printf("FILM_ALBUM_MOUNT_FAIL errno=%d\n",errno);
    rmdir(ALBUM_JOB_DIR);
    return -1;
}
static void album_release_mount(void) {
    for(int tries=0;tries<10;++tries) {
        unlink(A6400_PV_JOB_INPUT);
#ifdef __linux__
        if(umount(ALBUM_JOB_DIR)==0 || errno==EINVAL || errno==ENOENT)break;
        if(tries==9) {
            if(syscall(SYS_umount2,ALBUM_JOB_DIR,2)==0)break;
            printf("FILM_ALBUM_UMOUNT_FAIL errno=%d\n",errno);
        }
#else
        break;
#endif
        pause_ms(50);
    }
    for(int tries=0;tries<5;++tries) {
        if(rmdir(ALBUM_JOB_DIR)==0 || errno==ENOENT)return;
        pause_ms(20);
    }
    printf("FILM_ALBUM_RMDIR_FAIL errno=%d\n",errno);
}
static void album_cleanup_stale_mount(void) {
#ifdef __linux__
    FILE *mf=fopen("/proc/mounts","r");
    if(!mf)return;
    char line[256];int stale=0;
    while(fgets(line,sizeof(line),mf)) {
        if(strstr(line,"a6400-album " ALBUM_JOB_DIR " ")){stale=1;break;}
    }
    fclose(mf);
    if(stale)album_release_mount();
#endif
}
static int decode_test_jpeg(const char *name,uint32_t seq,uint32_t slot,int touch_current) {
    char source[40],ctmp[176],cdst[160];
    if(snprintf(source,sizeof(source),"A:/A6400/TEST/%s",name)!=26)return -1;
    if(pv_cache_path(name,cdst,sizeof(cdst))!=0)return -1;
    snprintf(ctmp,sizeof(ctmp),"%s.tmp",cdst);
    if(!album_request_matches(seq,slot))return -2;
    if(album_input_mount())return -1;
    pv_lru_evict_for(name);
    unlink(ctmp);
    char *reader[]={(char *)"/tmp_bt/a6400-film-native-command.elf",(char *)"readback",source,0};
    int rc=run_exec_until_left(reader[0],reader,seq,slot);
    if(!rc) {
        char *worker[]={(char *)"/tmp_bt/a6400-film-job.elf",(char *)"--album-preview",
            (char *)A6400_PV_JOB_INPUT,ctmp,(char *)name,0};
        rc=run_exec_until_left(worker[0],worker,seq,slot);
    }
    album_release_mount();
    if(rc) {
        unlink(ctmp);
        if(rc==-2 && touch_current)unlink(A6400_PV_CURRENT);
        return rc;
    }
    if(rename(ctmp,cdst)!=0) {
        unlink(ctmp);
        return -1;
    }
    pv_lru_touch(name);
    if(touch_current && pv_cache_activate(name)!=0)return -1;
    return 0;
}
static void remember_graded_name(const char *target) {
    const char *base=target?strrchr(target,'/'):NULL;
    if(!base || film_album_index_add(&g_album,base+1))return;
    if(!film_album_index_write(A6400_PV_ALBUM_INDEX,&g_album))publish_album_catalog(&g_album);
}
static int album_flip_waiting(void) {
    uint32_t mode=0,view_seq=0,view_slot=0,scan_req=0;
    if(read_album_request(&mode,&view_seq,&view_slot,&scan_req)!=0)return 0;
    unsigned pending=0;
    while(pending<queue_count && queue[pending].state!=PENDING)++pending;
    return film_album_preempts_scan(
        pending==queue_count && !any_waiting_candidate(),view_seq!=0,
        view_seq,view_slot,g_album_served_seq,g_album_served_slot,g_album.count,g_album_scanned,
        scan_req,g_album_scan_done,g_album_fail_seq,now_ms(),g_album_fail_ms);
}
static void service_test_album(struct transport *t,int idle) {
    if(!idle || !t)return;
    uint32_t mode=0,view_seq=0,view_slot=0,scan_req=0;
    int have_req=read_album_request(&mode,&view_seq,&view_slot,&scan_req)==0;
    (void)mode;
    if(!g_album_scanned || (have_req && scan_req!=g_album_scan_done)) {
        int first_scan=!g_album_scanned;
        if(t->pid<0 && transport_start(t)) {
            transport_stop(t);
            return;
        }
        static struct film_album_index fresh;
        int rc=inventory_test_album(t,&fresh);
        if(!rc) {
            g_album=fresh;
            if(mkdir(A6400_PV_DIR,0700) && errno!=EEXIST)rc=-1;
        }
        if(!rc && !film_album_index_write(A6400_PV_ALBUM_INDEX,&g_album)) {
            publish_album_catalog(&g_album);
            printf("FILM_ALBUM_SCAN count=%u truncated=%d\n",g_album.count,g_album.truncated);
        } else if(rc) {
            printf("FILM_ALBUM_SCAN_FAIL rc=%d\n",rc);
        }
        g_album_scanned=1;
        g_album_scan_done=scan_req;
        publish_album_ready(g_album_served_seq,g_album_served_slot,g_album_scan_done,NULL);
        /* Prewarm the single newest graded photo once after boot scan so even the
         * very first album open is 0ms instant from tmpfs RAM cache. */
        if(first_scan && !rc && g_album.count>0u) {
            const char *newest=g_album.names[g_album.count-1u];
            if(!pv_cache_has(newest) && album_request_matches(0u,0u)) {
                transport_stop(t);
                if(decode_test_jpeg(newest,0u,0u,0)==0)
                    printf("FILM_ALBUM_BOOT_WARM name=%s\n",newest);
            }
        }
    }
    uint32_t idle_since=now_ms();
    uint64_t last_sd_irq=0;
    int have_sd_irq=0;
#if FILM_CAPTURE_PROC_TRIGGER
    have_sd_irq=sd_io_sample(&last_sd_irq);
#endif
    for(;;) {
        if(daemon_signal || stop_requested()!=0)break;
        have_req=(read_album_request(&mode,&view_seq,&view_slot,&scan_req)==0);
        if(!have_req)break;
        if(scan_req!=g_album_scan_done) {
            if(t->pid<0 && transport_start(t)){transport_stop(t);break;}
            static struct film_album_index fresh2;
            int rc=inventory_test_album(t,&fresh2);
            if(!rc) {
                g_album=fresh2;
                if(mkdir(A6400_PV_DIR,0700) && errno!=EEXIST)rc=-1;
            }
            if(!rc && !film_album_index_write(A6400_PV_ALBUM_INDEX,&g_album)) {
                publish_album_catalog(&g_album);
                printf("FILM_ALBUM_SCAN count=%u truncated=%d\n",g_album.count,g_album.truncated);
            } else if(rc) {
                printf("FILM_ALBUM_SCAN_FAIL rc=%d\n",rc);
            }
            g_album_scanned=1;
            g_album_scan_done=scan_req;
            publish_album_ready(g_album_served_seq,g_album_served_slot,g_album_scan_done,NULL);
        }
        if(!view_seq || view_slot>=g_album.count)break;
        if(view_seq!=g_album_served_seq || view_slot!=g_album_served_slot) {
            const char *want_name=g_album.names[view_slot];
            if(pv_cache_activate(want_name)==0) {
                g_album_served_seq=view_seq;
                g_album_served_slot=view_slot;
                g_album_fail_seq=0;
                g_album_fail_count=0;
                publish_album_ready(view_seq,view_slot,g_album_scan_done,want_name);
                printf("FILM_ALBUM_CACHE_HIT slot=%u name=%s\n",view_slot,want_name);
                idle_since=now_ms();
                continue;
            }
            uint32_t now=now_ms();
            if(view_seq==g_album_fail_seq && (uint32_t)(now-g_album_fail_ms)<1000u) {
                if(!FILM_CAPTURE_PERSISTENT)break;
                pause_ms(25);
                continue;
            }
            transport_stop(t);
            int decoded=decode_test_jpeg(want_name,view_seq,view_slot,1);
            if(decoded==-2) {
                printf("FILM_ALBUM_PREVIEW_CANCEL slot=%u\n",view_slot);
                idle_since=now_ms();
                continue;
            }
            if(decoded) {
                g_album_fail_count=view_seq==g_album_fail_seq?g_album_fail_count+1u:1u;
                g_album_fail_seq=view_seq;
                g_album_fail_ms=now;
                printf("FILM_ALBUM_PREVIEW_FAIL slot=%u tries=%u\n",view_slot,g_album_fail_count);
                if(g_album_fail_count<3u)continue;
                unlink(A6400_PV_CURRENT);
                g_album_served_seq=view_seq;
                g_album_served_slot=view_slot;
#ifdef A6400_UI_LANG_SC
                publish_album_ready(view_seq,view_slot,g_album_scan_done,"读取失败");
#else
                publish_album_ready(view_seq,view_slot,g_album_scan_done,"讀取失敗");
#endif
                printf("FILM_ALBUM_PREVIEW_GIVEUP slot=%u name=%s\n",view_slot,want_name);
                idle_since=now_ms();
                continue;
            }
            g_album_served_seq=view_seq;
            g_album_served_slot=view_slot;
            g_album_fail_seq=0;
            g_album_fail_count=0;
            publish_album_ready(view_seq,view_slot,g_album_scan_done,want_name);
            printf("FILM_ALBUM_PREVIEW_OK slot=%u name=%s\n",view_slot,want_name);
            idle_since=now_ms();
#if FILM_CAPTURE_PROC_TRIGGER
            have_sd_irq=sd_io_sample(&last_sd_irq);
#endif
            continue;
        }
        /* Prefetch nearby uncached slots (-1, +1, -2, -3) while user views current photo. */
        int prefetched=0;
        static const int pf_offs[4]={-1,1,-2,-3};
        for(unsigned pi=0;pi<4u;++pi) {
            int64_t cand=(int64_t)view_slot+(int64_t)pf_offs[pi];
            if(cand<0 || (uint64_t)cand>=(uint64_t)g_album.count)continue;
            const char *pf_name=g_album.names[(unsigned)cand];
            if(pv_cache_has(pf_name))continue;
            transport_stop(t);
            int prc=decode_test_jpeg(pf_name,view_seq,view_slot,0);
            if(prc==0)printf("FILM_ALBUM_PREFETCH_OK slot=%u name=%s\n",(unsigned)cand,pf_name);
            prefetched=1;
            idle_since=now_ms();
#if FILM_CAPTURE_PROC_TRIGGER
            have_sd_irq=sd_io_sample(&last_sd_irq);
#endif
            break;
        }
        if(prefetched)continue;
        if(!FILM_CAPTURE_PERSISTENT || (uint32_t)(now_ms()-idle_since)>=15000u)break;
#if FILM_CAPTURE_PROC_TRIGGER
        if(have_sd_irq) {
            uint64_t cur_irq=0;
            if(sd_io_sample(&cur_irq) && cur_irq>=last_sd_irq && (cur_irq-last_sd_irq)>32u)break;
        }
#else
        (void)have_sd_irq;(void)last_sd_irq;
#endif
        pause_ms(15);
    }
}
#else
static void album_cleanup_stale_mount(void) {}
static void remember_graded_name(const char *target) {(void)target;}
static int album_flip_waiting(void) {return 0;}
static void service_test_album(struct transport *t,int idle) {(void)t;(void)idle;}
#endif

int main(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],"watch"))return 2;
#ifdef A6400_SOUND_LITE
    /* One-time MPR copy of SD /A6400/SOUNDS .pcm files -> RAM /tmp_bt/a6400-sounds/ */
    mkdir("/tmp_bt/a6400-sounds",0755);
    {
        struct transport t={.pid=-1,.in=-1,.out=-1};
        for(int attempt=0;attempt<30;++attempt) {
            if(!transport_start(&t)) {
                t.scan_started=now_ms();
                char names[12][64];
                int n_found=0, n_copied=0;
                struct item item;
                int first=1,res;
                while((res=native_item(&t,"ms:/A6400/SOUNDS",first,&item))==1) {
                    first=0;
                    if(item.directory || n_found>=12)continue;
                    size_t nl=strlen(item.name);
                    if(nl<5u || nl>=sizeof(names[0]))continue;
                    const char *ext=item.name+nl-4u;
                    if(ext[0]=='.' && (ext[1]=='p'||ext[1]=='P') &&
                       (ext[2]=='c'||ext[2]=='C') && (ext[3]=='m'||ext[3]=='M')) {
                        snprintf(names[n_found++],sizeof(names[0]),"%s",item.name);
                    }
                }
                transport_stop(&t);
                for(int i=0;i<n_found;++i) {
                    char src[64],dst[96];
                    snprintf(src,sizeof(src),"A:/A6400/SOUNDS/%.44s",names[i]);
                    snprintf(dst,sizeof(dst),"/tmp_bt/a6400-sounds/%.44s",names[i]);
                    unlink("/tmp_bt/a6400-custom-charo.pcm");
                    char *rd[]={(char *)"/tmp_bt/a6400-film-native-command.elf",(char *)"sound",src,0};
                    pid_t cp=fork();
                    if(cp==0) { execv(rd[0],rd); _exit(127); }
                    if(cp>0) {
                        int st=0;
                        if(waitpid(cp,&st,0)==cp && WIFEXITED(st) && WEXITSTATUS(st)==0) {
                            if(rename("/tmp_bt/a6400-custom-charo.pcm",dst)==0) n_copied++;
                        }
                    }
                }
                {
                    FILE *lf=fopen("/tmp_bt/a6400-sound-scan.log","w");
                    if(lf){fprintf(lf,"attempt=%d res=%d found=%d copied=%d\n",attempt,res,n_found,n_copied);fclose(lf);}
                }
                if(n_copied>0)break;
            }
            sleep(2);
        }
    }
    for(;;) {
        if(access("/tmp_bt/a6400-film.DISABLE",F_OK)==0 ||
           access("/tmp_bt/a6400-film-capture.stop",F_OK)==0) return 0;
        sleep(2);
    }
#endif
    /* Startup stdout can already contain bootstrap/guard evidence. Own a
     * separate fresh file; never adopt or truncate that preceding stream. */
    if(film_log_open(&capture_log,FILM_CAPTURE_RUNTIME_LOG,1))return 22;
    setvbuf(stdout,NULL,_IONBF,0);
    if(!pins()){puts("FILM_CAPTURE_FAIL reason=pin");return 3;}
    if(stop_requested()!=0 || !absent_file(FILM_CAPTURE_INFLIGHT) ||
       !absent_file(FILM_CAPTURE_JOB_LOCK)) {puts("FILM_CAPTURE_FAIL reason=stale_stop_or_job");return 5;}
    int fd=open(CAPTURE_LOCK,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0){puts("FILM_CAPTURE_FAIL reason=lock");return 4;}
    char pid[40];int n=snprintf(pid,sizeof(pid),"pid=%u\n",(unsigned)getpid());
    int lock_bad=write(fd,pid,(size_t)n)!=n;close(fd);
    if(lock_bad){unlink(CAPTURE_LOCK);return 5;}
    signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    signal(SIGINT,daemon_stop);signal(SIGTERM,daemon_stop);
    struct transport transport={.pid=-1,.in=-1,.out=-1};static struct catalogue current;
    int result=1;const char *reason="error";uint32_t start=now_ms();
    next_output=(start^((uint32_t)getpid()*2654435761u))%1000u;
    if(save_manifest()){reason="manifest";goto done;}
    printf("FILM_CAPTURE_READY active_ms=%u persistent=%d queue_capacity=%u\n",
           FILM_CAPTURE_ACTIVE_MS,FILM_CAPTURE_PERSISTENT,MAX_FILES);
    printf("FILM_CAPTURE_FAST latest=%d proc=%d wait=%u int=%u stb=%u hb=%u\n",
           FILM_CAPTURE_LATEST_DIR_ONLY,FILM_CAPTURE_PROC_TRIGGER,CAPTURE_WAIT_MS,
           CAPTURE_INTERVAL_MS,CAPTURE_STABLE_MS,FILM_CAPTURE_IDLE_HEARTBEAT_MS);
    if(capture_log_bad()){result=21;retain_lock=1;reason="runtime_log";goto done;}
    if(transport_start(&transport)){result=5;reason="monitor_start";goto done;}
    uint32_t baseline_started=now_ms();int baseline;unsigned metadata_failures=0;
    for(;;) {
        if(capture_log_bad()){result=21;retain_lock=1;reason="runtime_log";goto done;}
        int stop=stop_requested();
        if(stop<0){reason="stop_marker";goto done;}
        if(daemon_signal){result=20;reason="signal";goto done;}
        if(stop){result=0;reason="stop_file";goto done;}
        baseline=inventory_dirs(&transport,&current,FILM_CAPTURE_LATEST_DIR_ONLY);
        if(capture_log_bad()){result=21;retain_lock=1;reason="runtime_log";goto done;}
        if(baseline==-1 && transport.metadata_retryable) {
            if(!metadata_retry(&metadata_failures,"baseline") ||
               (uint32_t)(now_ms()-baseline_started)>=FILM_CAPTURE_BASELINE_TIMEOUT_MS)break;
            phase="baseline_metadata_wait";
            if(save_manifest()){reason="manifest";goto done;}
            pause_interruptible(CAPTURE_BASELINE_INTERVAL_MS,start,0);continue;
        }
        if(baseline!=-2)break;
        /* A complete native reply explicitly reported unmounted media. No
         * partial inventory is accepted as an empty baseline, and unknown
         * failures are never retried. Cold boot may precede SD readiness. */
        if((uint32_t)(now_ms()-baseline_started)>=FILM_CAPTURE_BASELINE_TIMEOUT_MS) {
            reason="baseline_media_timeout";goto done;
        }
        phase="baseline_media_wait";
        if(save_manifest()){reason="manifest";goto done;}
        printf("FILM_CAPTURE_BASELINE_MEDIA_WAIT elapsed_ms=%u limit_ms=%u\n",
               (unsigned)(now_ms()-baseline_started),FILM_CAPTURE_BASELINE_TIMEOUT_MS);
        pause_interruptible(CAPTURE_BASELINE_INTERVAL_MS,start,0);
    }
    if(baseline) {
        result=21;retain_lock=1;reason="baseline_unconfirmed";goto done;
    }
    metadata_resumed(&metadata_failures,"baseline");
    if(current.count>=MAX_FILES){reason="baseline_capacity";goto done;}
    for(unsigned i=0;i<current.count;++i) {
        char source[40];if(source_path(&current.items[i],source,sizeof(source))){reason="baseline_path";goto done;}
        queue[i].entry=current.items[i];queue[i].state=OLD;queue[i].present=1;
    }
    queue_count=current.count;phase="waiting";
    if(save_manifest()){reason="manifest";goto done;}
    printf("FILM_CAPTURE_BASELINE count=%u\n",queue_count);
    sync_custom_charo(&transport);
    start_preview_subsystem();
    push_ui_status(0u);
#if CAPTURE_WAIT_MS > 0u
    uint32_t waiting=now_ms();
    while(!daemon_signal && !stop_requested() &&
          (uint32_t)(now_ms()-waiting)<CAPTURE_WAIT_MS)pause_ms(10);
#endif
    start=now_ms();
    int post_trigger_follow=0,early_toast_sent=0;
    (void)early_toast_sent;
    unsigned batch_done=0;
    int dexp_first_valid=0;
    char dexp_first_path[40]="";
    unsigned dexp_first_bytes=0u;
    char latest_dcim_dir[48]="";
    update_latest_dir(latest_dcim_dir,sizeof(latest_dcim_dir),&current);
    while(1) {
        if(capture_log_bad()){result=21;retain_lock=1;reason="runtime_log";goto done;}
        int stop=stop_requested();
        if(stop<0){reason="stop_marker";break;}
        if(daemon_signal){result=20;reason="signal";break;}
        if(stop){result=0;reason="stop_file";break;}
        if(dexp_first_valid) {
            uint32_t cur_pending=0u;
            uint32_t cur_mode=read_ui_dexp(NULL,&cur_pending);
            if(cur_mode==0u || cur_pending==0u) {
                dexp_first_valid=0;dexp_first_path[0]=0;dexp_first_bytes=0u;
                push_v4_dexp_pending(0u);
                puts(cur_mode==0u?"FILM_DEXP_CANCELLED_IDLE":"FILM_DEXP_CANCELLED_SHOT1");
            }
        }
        if(!FILM_CAPTURE_PERSISTENT && (uint32_t)(now_ms()-start)>=FILM_CAPTURE_ACTIVE_MS) {
            result=0;reason="expired";break;
        }
        if(album_flip_waiting()) {
            service_test_album(&transport,1);
            continue;
        }
        if(transport.pid<0 && transport_start(&transport)) {
#if FILM_CAPTURE_PERSISTENT
            transport_stop(&transport);
            pause_interruptible(500u,start,1);
            continue;
#else
            reason="monitor_restart";break;
#endif
        }
        int use_hint=FILM_CAPTURE_LATEST_DIR_ONLY && any_waiting_candidate() && latest_dcim_dir[0];
        phase="scanning";int scan=inventory_dirs_with_hint(&transport,&current,FILM_CAPTURE_LATEST_DIR_ONLY,use_hint?latest_dcim_dir:NULL);
        if(capture_log_bad()){result=21;retain_lock=1;reason="runtime_log";goto done;}
        if(scan==-2) {
            latest_dcim_dir[0]=0;early_toast_sent=0;
            reset_candidate_stability();post_trigger_follow=0;
            phase="media_unavailable";
#if FILM_CAPTURE_PERSISTENT
            /* Release mpr_monio while SD media is unmounted (e.g. USB Mass Storage). */
            transport_stop(&transport);
#endif
            if(save_manifest()){reason="manifest";break;}
#ifdef FILM_CAPTURE_MEDIA_UNAVAILABLE_PAUSE_MS
            pause_interruptible(FILM_CAPTURE_MEDIA_UNAVAILABLE_PAUSE_MS,start,1);continue;
#else
            pause_interruptible(FILM_CAPTURE_PERSISTENT?2000u:CAPTURE_INTERVAL_MS,start,1);continue;
#endif
        }
        if(scan==-1 && transport.metadata_retryable) {
            latest_dcim_dir[0]=0;early_toast_sent=0;
            reset_candidate_stability();post_trigger_follow=0;
            if(!metadata_retry(&metadata_failures,"watch")) {
#if FILM_CAPTURE_PERSISTENT
                metadata_failures=0;
                transport_stop(&transport);
                pause_interruptible(200u,start,1);
                continue;
#else
                result=21;retain_lock=1;reason="scan_unconfirmed";break;
#endif
            }
            phase="metadata_wait";
            if(save_manifest()){reason="manifest";break;}
            pause_interruptible(CAPTURE_INTERVAL_MS,start,1);continue;
        }
        if(scan) {
#if FILM_CAPTURE_PERSISTENT
            latest_dcim_dir[0]=0;early_toast_sent=0;
            reset_candidate_stability();post_trigger_follow=0;
            transport_stop(&transport);
            pause_interruptible(500u,start,1);
            continue;
#else
            result=21;retain_lock=1;reason="scan_unconfirmed";break;
#endif
        }
        metadata_resumed(&metadata_failures,"watch");
        if(current.count)update_latest_dir(latest_dcim_dir,sizeof(latest_dcim_dir),&current);
        else latest_dcim_dir[0]=0;
        if(update_queue(&current)){reason="queue_capacity_or_path";break;}
        if(save_manifest()){reason="manifest";break;}
        unsigned pending=0;
        while(pending<queue_count && queue[pending].state!=PENDING)++pending;
#if FILM_CAPTURE_PERSISTENT && FILM_CAPTURE_PROC_TRIGGER
        if(pending==queue_count && any_waiting_candidate()) {
#ifdef FILM_DAEMON_TOAST_RVA
            if(!early_toast_sent) {
                char settings[128];
                if(live_settings(settings,sizeof(settings))==0) {
                    unsigned active=count_active_queue();
                    push_toast_status(1u,25000u,settings,NULL,batch_done+1u,batch_done+active,read_ui_hires());
                    early_toast_sent=1;
                }
            }
#endif
            uint64_t q0=0,q1=0;
            if(sd_io_sample(&q0)) {
                pause_interruptible(100u,start,1);
                if(!should_break_wait(start,1) && sd_io_sample(&q1) && q1>=q0 && (q1-q0)<=4u) {
                    int promoted=0;
                    for(unsigned i=0;i<queue_count;++i) {
                        if(queue[i].present && queue[i].state==WAITING && queue[i].entry.size>0u) {
                            char source[40];
                            if(!source_path(&queue[i].entry,source,sizeof(source))) {
                                queue[i].state=PENDING;
                                printf("FILM_CAPTURE_PENDING source=%s bytes=%u heuristic_only=1 uptime_ms=%u\n",
                                       source,queue[i].entry.size,(unsigned)now_ms());
                                promoted=1;
                            }
                        }
                    }
                    if(promoted) {
                        if(save_manifest()){reason="manifest";break;}
                        pending=0;
                        while(pending<queue_count && queue[pending].state!=PENDING)++pending;
                    }
                }
            }
        }
#endif
        if(pending<queue_count) {
            /* inventory returned a fresh completed native prompt. Reap MPR
             * before the settings CLI or any IFSYS read/write can begin. */
            transport_stop(&transport);post_trigger_follow=0;
            if(daemon_signal || stop_requested()!=0)continue;
            struct queued *q=&queue[pending];char settings[128];
            uint32_t cur_dp=0u;
            uint32_t dmode=read_ui_dexp(NULL,&cur_dp);
            if(dexp_first_valid && (dmode==0u || cur_dp==0u)) {
                dexp_first_valid=0;dexp_first_path[0]=0;dexp_first_bytes=0u;
                push_v4_dexp_pending(0u);
                puts("FILM_DEXP_CANCELLED_SHOT1");
            }
            if(dmode!=0u) {
                /* Double Exposure mode is active (strictly mutually exclusive with LUT). */
                char src_cur[40];
                if(source_path(&q->entry,src_cur,sizeof(src_cur))==0) {
                    if(!dexp_first_valid) {
                        snprintf(dexp_first_path,sizeof(dexp_first_path),"%s",src_cur);
                        dexp_first_bytes=q->entry.size;
                        dexp_first_valid=1;
                        q->state=OFF;++off_jobs;
                        push_v4_dexp_pending(1u);
                        publish_v4_shot(0,NULL,NULL,0);
                        char tmsg[64];
#ifdef A6400_UI_LANG_SC
                        snprintf(tmsg,sizeof(tmsg),"双重曝光 1/2 已拍 (%s) — 请拍第2张",dexp_ui_name(dmode));
#else
                        snprintf(tmsg,sizeof(tmsg),"雙重曝光 1/2 已拍 (%s) — 請拍第2張",dexp_ui_name(dmode));
#endif
                        push_ui_toast(1u,8000u,tmsg);
                        printf("FILM_DEXP_SHOT1 source=%s bytes=%u mode=%s uptime_ms=%u\n",
                               dexp_first_path,dexp_first_bytes,dexp_arg_name(dmode),(unsigned)now_ms());
                    } else {
                        /* Always blend 1.7MP embedded previews (~10 MiB total RAM) to avoid
                         * camera OOM decode failures when user LUT quality is 6MP/12MP. */
                        int eff_hires=0;
                        make_target_path(&q->entry,next_output,q->target,sizeof(q->target));
                        next_output=(next_output+1u)%1000u;
                        push_v4_dexp_pending(2u);
                        push_ui_status(1u);
                        char tmsg[64];
#ifdef A6400_UI_LANG_SC
                        snprintf(tmsg,sizeof(tmsg),"双曝 2/2 合成中 (%s%s)...",dexp_ui_name(dmode),eff_hires?" 6MP":"");
#else
                        snprintf(tmsg,sizeof(tmsg),"雙曝 2/2 合成中 (%s%s)...",dexp_ui_name(dmode),eff_hires?" 6MP":"");
#endif
                        push_ui_toast(1u,eff_hires?45000u:18000u,tmsg);
                        early_toast_sent=0;
                        unsigned prev_applied=applied_jobs;
                        album_cleanup_stale_mount();
                        int jrc=run_dexp_job(dexp_first_path,dexp_first_bytes,q,dmode,eff_hires);
                        dexp_first_valid=0;dexp_first_path[0]=0;dexp_first_bytes=0u;
                        push_v4_dexp_pending(0u);
                        if(jrc) {
#ifdef A6400_UI_LANG_SC
                            push_ui_toast(3u,3500u,"双曝合成失败 (未确认)");
#else
                            push_ui_toast(3u,3500u,"雙曝合成失敗 (未確認)");
#endif
                            push_ui_status(0u);
                            result=21;retain_lock=1;reason="job_uncertain";break;
                        }
                        int did_apply=(applied_jobs>prev_applied);
                        char dcap[48];
                        snprintf(dcap,sizeof(dcap),"preset=dexp_%s",dexp_arg_name(dmode));
                        publish_v4_shot(did_apply,dcap,q->target,eff_hires);
                        if(did_apply)remember_graded_name(q->target);
                        const char *base=strrchr(q->target,'/');
                        base=base?base+1:q->target;
#ifdef A6400_UI_LANG_SC
                        if(did_apply)snprintf(tmsg,sizeof(tmsg),"双曝 2/2 完成 (%s %.14s)",dexp_ui_name(dmode),base);
                        else snprintf(tmsg,sizeof(tmsg),"双曝 2/2 失败: %.32s",last_job_fail_hint[0]?last_job_fail_hint:dexp_ui_name(dmode));
#else
                        if(did_apply)snprintf(tmsg,sizeof(tmsg),"雙曝 2/2 完成 (%s %.14s)",dexp_ui_name(dmode),base);
                        else snprintf(tmsg,sizeof(tmsg),"雙曝 2/2 失敗: %.32s",last_job_fail_hint[0]?last_job_fail_hint:dexp_ui_name(dmode));
#endif
                        push_ui_toast(did_apply?2u:3u,4000u,tmsg);
                        push_ui_status(count_active_queue());
                        post_trigger_follow=1;
                    }
                    if(save_manifest()){reason="manifest";break;}
                    wait_before_scan(start,&post_trigger_follow);
                    continue;
                }
            } else if(dexp_first_valid) {
                dexp_first_valid=0;dexp_first_path[0]=0;dexp_first_bytes=0u;
                push_v4_dexp_pending(0u);
            }
            int gate=live_settings(settings,sizeof(settings));
            if(daemon_signal || stop_requested()!=0)continue;
            if(gate==10) {
                early_toast_sent=0;batch_done=0;
                /* This fresh OFF observation applies to the entire pending
                 * snapshot, not only its first entry. Later ON must not turn
                 * the rest of this already observed OFF batch into jobs. */
                for(unsigned i=0;i<queue_count;++i)if(queue[i].present && queue[i].state==PENDING) {
                    queue[i].state=OFF;++off_jobs;
                    publish_v4_shot(0,NULL,NULL,0);
                    printf("FILM_CAPTURE_SKIP_OFF path=%s\n",queue[i].entry.path);
                }
            } else if(gate) {
                early_toast_sent=0;
                puts("FILM_CAPTURE_SETTINGS_UNAVAILABLE");
            } else {
                int hires=read_ui_hires();
                unsigned active=count_active_queue();
                unsigned bpos=batch_done+1u,btot=batch_done+(active?active:1u);
                make_target_path(&q->entry,next_output,q->target,sizeof(q->target));
                next_output=(next_output+1u)%1000u;
                printf("FILM_CAPTURE_DISPATCH path=%s target=%s %s hires=%d q=%u/%u uptime_ms=%u\n",
                       q->entry.path,q->target,settings,hires,bpos,btot,(unsigned)now_ms());
                push_toast_status(1u,hires==2?75000u:(hires==1?45000u:15000u),settings,NULL,bpos,btot,hires);
                push_ui_status(active?active:1u);
                early_toast_sent=0;
                unsigned prev_applied=applied_jobs;
                album_cleanup_stale_mount();
                if(run_job(q,settings,hires)) {
#ifdef A6400_UI_LANG_SC
                    push_ui_toast(3u,3000u,"处理失败 (未确认)");
#else
                    push_ui_toast(3u,3000u,"處理失敗 (未確認)");
#endif
                    push_ui_status(0u);
                    result=21;retain_lock=1;reason="job_uncertain";break;
                }
                int did_apply=(applied_jobs>prev_applied);
                publish_v4_shot(did_apply,settings,q->target,hires);
                if(did_apply)remember_graded_name(q->target);
                push_toast_status(did_apply?2u:3u,
                                  did_apply?3500u:3500u,settings,q->target,bpos,btot,hires);
                push_ui_status(count_active_queue());
                if(did_apply)++batch_done;
                if(count_active_queue()==0u)batch_done=0;
                /* Recheck promptly in case another shutter press arrived during render. */
                post_trigger_follow=1;
            }
            if(save_manifest()){reason="manifest";break;}
        } else if(any_waiting_candidate()) {
#ifdef FILM_DAEMON_TOAST_RVA
            if(!early_toast_sent) {
                char settings[128];
                if(live_settings(settings,sizeof(settings))==0) {
                    unsigned active=count_active_queue();
                    push_toast_status(1u,25000u,settings,NULL,batch_done+1u,batch_done+active,read_ui_hires());
                    early_toast_sent=1;
                }
            }
#endif
        } else {
            early_toast_sent=0;
            batch_done=0;
        }
        service_test_album(&transport,pending==queue_count && !any_waiting_candidate());
        wait_before_scan(start,&post_trigger_follow);
    }
done:
    stop_preview_subsystem();
    transport_stop(&transport);
    phase=retain_lock?"uncertain":reason;
    if(own_manifest && save_manifest()){result=21;retain_lock=1;}
    if(!retain_lock && unlink(CAPTURE_LOCK)){result=21;retain_lock=1;}
    printf("FILM_CAPTURE_STOP result=%d reason=%s applied=%u off=%u failed=%u retained=%d pid=%u\n",
           result,reason,applied_jobs,off_jobs,failed_jobs,retain_lock,(unsigned)getpid());
    return film_log_close(&capture_log)?21:result;
}
