#!/usr/bin/env python3
"""Production durable-intent journal/finalize tests with a small JSON test double.

The cJSON double supplies known scalar fixture fields. The production journal
reader selects complete records and the production writer performs real FILE I/O.
"""
from session_storage_behavior_test import COMMON, DEFS, SW, function, run

code=COMMON+r'''
#include <dirent.h>
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef struct session_writer session_writer_t;
'''+DEFS+r'''
#define SD_APP_DIR "."
#define SD_PENDING_EXPORT_DIR "./pending_export"
#define SD_LEASE_EXPORT 1
static int sync_fail,unlink_fail,closed,lease_depth,manifests;
static bool allow_lease=true;
static unsigned nonce_counter;
static void esp_fill_random(void *p,size_t len) {uint32_t *v=p;for(size_t i=0;i<len/4;i++)v[i]=++nonce_counter;}

static int fake_sync(int fd) {(void)fd;if(sync_fail){errno=EIO;return -1;}return 0;}
static int fake_unlink(const char *path) {if(unlink_fail){errno=EIO;return -1;}return unlink(path);}
static int count_close(FILE *f) {closed++;return fclose(f);}
#define fsync fake_sync
#define unlink fake_unlink
#define fclose count_close
static bool sd_storage_is_ready(void) {return true;}
static bool sd_storage_lease_acquire(int a,int b) {(void)a;(void)b;if(!allow_lease)return false;lease_depth++;return true;}
static void sd_storage_lease_release(int a) {(void)a;assert(lease_depth>0);lease_depth--;}
static void sd_storage_lease_release_unchanged(int a) {sd_storage_lease_release(a);}
static const char *esp_err_to_name(int e) {(void)e;return "ESP_FAIL";}
// Scalar cJSON test double; malformed or unfinished fixtures have no object.
typedef struct cJSON {int type;double valuedouble;char *valuestring;struct cJSON *children;} cJSON;
static cJSON *cJSON_Parse(const char *line) {
    const char *d=strstr(line,"\"day\":\"");if(!d || !strchr(d,'}'))return NULL;
    cJSON *r=calloc(1,sizeof *r);r->children=calloc(6,sizeof *r);
    cJSON *v=r->children;v[0].type=1;v[0].valuestring=calloc(16,1);
    if(sscanf(d,"\"day\":\"%15[^\"]",v[0].valuestring)!=1){free(v[0].valuestring);free(v);free(r);return NULL;}
    d=strstr(line,"\"attempts\":");v[1].type=2;v[1].valuedouble=d?strtol(d+11,NULL,10):0;
    d=strstr(line,"\"stalled\":true");v[2].type=d?3:0;
    d=strstr(line,"\"last_error\":\"");if(d){v[3].type=1;v[3].valuestring=calloc(96,1);sscanf(d,"\"last_error\":\"%95[^\"]",v[3].valuestring);}
    d=strstr(line,"\"phase\":\"");if(d){v[4].type=1;v[4].valuestring=calloc(32,1);sscanf(d,"\"phase\":\"%31[^\"]",v[4].valuestring);}
    d=strstr(line,"\"generation\":\"");if(d){v[5].type=1;v[5].valuestring=calloc(40,1);sscanf(d,"\"generation\":\"%39[^\"]",v[5].valuestring);}
    return r;
}
static cJSON *cJSON_GetObjectItem(const cJSON *r,const char *name) {
    if(!r)return NULL;int i=!strcmp(name,"day")?0:!strcmp(name,"attempts")?1:!strcmp(name,"stalled")?2:!strcmp(name,"phase")?4:!strcmp(name,"generation")?5:3;
    return r->children+i;
}
static bool cJSON_IsString(const cJSON *v){return v && v->type==1;}
static bool cJSON_IsNumber(const cJSON *v){return v && v->type==2;}
static bool cJSON_IsTrue(const cJSON *v){return v && v->type==3;}
static void cJSON_Delete(cJSON *r){if(r){free(r->children[0].valuestring);free(r->children[3].valuestring);free(r->children[4].valuestring);free(r->children[5].valuestring);free(r->children);free(r);}}
'''
code+=''.join(function(SW,n) for n in ['pending_path','pending_export_mark_session',
    'pending_export_ack_session','pending_read','pending_upload_ready','pending_mark_upload_ready',
    'session_writer_mark_upload_invalidation','pending_key_valid',
    'session_writer_next_upload_invalidation','session_writer_ack_upload_invalidation',
    'pending_reason','pending_update','pending_error_is_permanent'])
code+=r'''
static int64_t esp_timer_get_time(void){return 1000000;}
static void storage_write_batch(session_writer_t *s,stream_batch_t *b){(void)s;(void)b;}
static void batch_reset(stream_batch_t *b){(void)b;}
static void storage_commit(session_writer_t *s){(void)s;}
static void io_fail(session_writer_t *s,const char *v){(void)v;s->storage_failed=true;}
static void noon_day_folder_local(time_t t,char *out,size_t n){(void)t;snprintf(out,n,"20260906");}
static void as11_time_noon_day(int64_t t,char *out,size_t n){(void)t;snprintf(out,n,"20260906");}
static void write_manifest(session_writer_t *s,const char *st){
    (void)st;assert(!s->files_open);assert(!s->flow.f_l0 && !s->f_events && !s->f_ckpt);
    char p[160];pending_path(s->session_id,p,sizeof p);assert(access(p,F_OK)==0);manifests++;
}
static void sd_storage_recording_end(void){}
static void lat_report(void){}
static void batch_pool_destroy(session_writer_t *s){(void)s;}
static void vSemaphoreDelete(void *p){(void)p;}
'''
code+=function(SW,'close_session_file')+function(SW,'storage_finalize')
code+=r'''
static void check(const char *id,int want,bool needs) {
    char p[160],day[16];pending_path(id,p,sizeof p);int attempts;bool stalled;
    pending_read(p,&attempts,&stalled,day);assert(!strcmp(day,"20260906"));
    assert(attempts==want && stalled==needs);
}
int main(void) {
    // Finalization closes raw data, then durable intent, then terminal manifest.
    session_writer_t s={.files_open=true};snprintf(s.session_id,sizeof s.session_id,"first");
    s.flow.f_l0=tmpfile();s.f_events=tmpfile();s.f_ckpt=tmpfile();
    sw_cmd_t cmd={.state="completed"};storage_finalize(&s,&cmd);
    assert(manifests==1);check("first",0,false);
    // Failed marker sync prevents terminal publication; recovery retains its work.
    memset(&s,0,sizeof s);snprintf(s.session_id,sizeof s.session_id,"failed");
    sync_fail=1;closed=0;storage_finalize(&s,&cmd);
    assert(s.storage_failed && manifests==1 && closed==1);
    assert(access("./pending_export/failed.json",F_OK)!=0);sync_fail=0;
    assert(pending_export_mark_session("second","20260906"));
    char p[160];pending_path("first",p,sizeof p);
    pending_update(p,"20260906",1,false,ESP_FAIL);check("first",1,false);
    // Simulate reset halfway through appending a new record; prior line wins.
    FILE *f=fopen(p,"a");fputs("{\"day\":\"20260906\",\"attempts\":9",f);fclose(f);
    check("first",1,false);
    pending_update(p,"20260906",2,true,EDF_GEN_ERR_POSITION_GAPS);check("first",2,true);
    char reason[96];pending_reason(p,reason,sizeof reason);
    assert(!strcmp(reason,"positioned_gaps_require_discontinuous_export"));
    assert(pending_error_is_permanent(EDF_GEN_ERR_POSITION_GAPS));
    // Idempotent marker creation does not erase retry metadata after reboot.
    assert(pending_export_mark_session("first","20260906"));check("first",2,true);
    allow_lease=false;pending_export_ack_session("first");check("first",2,true);
    allow_lease=true;pending_export_ack_session("first");assert(access(p,F_OK)!=0);
    check("second",0,false);assert(lease_depth==0);
    assert(session_writer_mark_upload_invalidation("20260906")==ESP_OK);
    char token[128],again[128];uint32_t day;
    assert(session_writer_next_upload_invalidation(&day,token,sizeof token) && day==20260906);
    // No RAM callback/queue state is needed to rediscover the same generation.
    assert(session_writer_next_upload_invalidation(&day,again,sizeof again));assert(!strcmp(token,again));
    allow_lease=false;assert(session_writer_ack_upload_invalidation(day,token)==ESP_ERR_TIMEOUT);
    allow_lease=true;assert(session_writer_next_upload_invalidation(&day,again,sizeof again));
    // A new publication between next and ack cannot be erased by the old token.
    assert(session_writer_mark_upload_invalidation("20260906")==ESP_OK);
    assert(session_writer_ack_upload_invalidation(day,token)==ESP_ERR_INVALID_STATE);
    assert(session_writer_next_upload_invalidation(&day,again,sizeof again));assert(strcmp(token,again));
    assert(session_writer_ack_upload_invalidation(20260905,again)==ESP_ERR_INVALID_STATE);
    // An interrupted newer append preserves the last complete handoff token.
    char handoff[160];pending_path("rebuild_20260906",handoff,sizeof handoff);
    f=fopen(handoff,"a");fputs("{\"day\":\"20260906\",\"phase\":\"upload_pending\"",f);fclose(f);
    assert(session_writer_next_upload_invalidation(&day,token,sizeof token));assert(!strcmp(token,again));
    // A failed durable acknowledgement never consumes the work, even when
    // the uploader has already deleted its old index and will repeat safely.
    unlink_fail=1;assert(session_writer_ack_upload_invalidation(day,again)==ESP_FAIL);
    unlink_fail=0;assert(session_writer_next_upload_invalidation(&day,token,sizeof token));assert(!strcmp(token,again));
    assert(session_writer_ack_upload_invalidation(day,again)==ESP_OK);
    assert(!session_writer_next_upload_invalidation(&day,again,sizeof again));
    assert(lease_depth==0);
    unlink("./pending_export/second.json");rmdir("./pending_export");
    puts("production durable export intent: finalization, failed sync/unlink, torn journals, reboot and generation-fenced acknowledgement passed");
}
'''
if __name__=='__main__':
    run('pending_intent_test',code)
