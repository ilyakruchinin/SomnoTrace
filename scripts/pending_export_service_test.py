#!/usr/bin/env python3
"""Production retry selection and persistent scheduler handoff interleavings."""
from pending_export_behavior_test import code as pending_code
from session_storage_behavior_test import SW, function, run

code = pending_code.split('int main(void) {')[0] + r'''
#define SD_STREAMS_DIR "./raw"
#define PENDING_MAX_ATTEMPTS 12
#define PENDING_MIN_FREE_HEAP (12 * 1024)
#define MALLOC_CAP_INTERNAL 1
static bool s_deferred_export_enabled=true;
static session_writer_t *s_active;
static int generated, rejected_nudges, dir_index, missing_fault;
static char last_rebuilt[16];
static bool sd_storage_recording_active(void){return false;}
static bool sd_storage_recording_pending(void){return false;}
static size_t heap_caps_get_free_size(int cap){(void)cap;return 100000;}
static void crash_diag_note_activity(const char *s){(void)s;}
static esp_err_t edf_gen_rebuild_day(const char *day){
    generated++;snprintf(last_rebuilt,sizeof last_rebuilt,"%s",day);
    // This durable hook is also executed by the actual shared rebuild path;
    // edf_rebuild_behavior_test checks its success/failure sentinel ordering.
    return session_writer_mark_upload_invalidation(day);
}
static void uploader_request_scan(void){rejected_nudges++; /* saturated scheduler queue */}
static DIR *ordered_open(const char *path){(void)path;dir_index=0;return (DIR*)1;}
static struct dirent *ordered_read(DIR *dir){
    (void)dir;static struct dirent e;
    const char *names[]={"missing-key.json","rebuild_20260906.json","healthy-key.json","newer-key.json"};
    if(dir_index==4)return NULL;
    memset(&e,0,sizeof e);strcpy(e.d_name,names[dir_index++]);return &e;
}
static int ordered_close(DIR *dir){(void)dir;return 0;}
static int source_stat(const char *path,struct stat *out){
    if(missing_fault && !strcmp(path,"./raw/20260905")){errno=EIO;return -1;}
    return stat(path,out);
}
static time_t fixed_time(time_t *out){time_t now=1800000000;if(out)*out=now;return now;}
#define opendir ordered_open
#define readdir ordered_read
#define closedir ordered_close
#define stat(...) source_stat(__VA_ARGS__)
#define time fixed_time
'''
code += function(SW,'pending_export_service_unlocked')+function(SW,'pending_export_service')
code += r'''
#undef opendir
#undef readdir
#undef closedir
#undef stat
#undef time
int main(void){
    mkdir(SD_STREAMS_DIR,0700);mkdir(SD_STREAMS_DIR "/20260906",0700);
    assert(pending_export_mark_session("missing-key","20260905"));
    assert(pending_export_mark_session("healthy-key","20260906"));
    pending_export_service();
    assert(generated==1 && !strcmp(last_rebuilt,"20260906") && rejected_nudges==1);
    assert(access("./pending_export/healthy-key.json",F_OK)!=0);
    assert(access("./pending_export/missing-key.json",F_OK)==0);
    uint32_t day;char token[128],token_after_restart[128];
    assert(session_writer_next_upload_invalidation(&day,token,sizeof token) && day==20260906);
    // Queue delivery was rejected, but the last authoritative work survived.
    assert(access("./pending_export/rebuild_20260906.json",F_OK)==0);
    struct stat before,after;assert(stat("./pending_export/missing-key.json",&before)==0);
    for(int i=0;i<100;i++)pending_export_service();
    assert(generated==1);assert(stat("./pending_export/missing-key.json",&after)==0);
    assert(before.st_size==after.st_size); // repeated absence cannot grow the journal forever
    assert(session_writer_next_upload_invalidation(&day,token_after_restart,sizeof token_after_restart));
    assert(!strcmp(token,token_after_restart)); // filesystem-only rediscovery; no queued state
    int attempts;bool stalled;char day_text[16];
    pending_read("./pending_export/missing-key.json",&attempts,&stalled,day_text);
    assert(attempts==0 && !stalled && !strcmp(day_text,"20260905"));
    // Neither missing source nor unacknowledged upload_pending monopolizes work.
    mkdir(SD_STREAMS_DIR "/20260907",0700);
    assert(pending_export_mark_session("newer-key","20260907"));
    missing_fault=1;pending_export_service();
    assert(generated==2 && !strcmp(last_rebuilt,"20260907"));
    assert(access("./pending_export/missing-key.json",F_OK)==0);
    // Returning source becomes eligible without resetting or deleting its intent.
    missing_fault=0;mkdir(SD_STREAMS_DIR "/20260905",0700);pending_export_service();
    assert(generated==3 && !strcmp(last_rebuilt,"20260905"));
    assert(access("./pending_export/missing-key.json",F_OK)!=0);
    assert(access("./pending_export/rebuild_20260905.json",F_OK)==0);
    assert(lease_depth==0);
    puts("production retry service: missing-first/EIO fairness, restored-source retry, saturated wakeup and reboot-safe handoff passed");
}
'''
if __name__ == '__main__':
    run('pending_service',code)
