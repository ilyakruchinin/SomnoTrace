/* Execute the production SMB diagnostic against deterministic transport fakes. */
#include "uploader.h"
#include "smb2.h"
#include "lwip/netdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
static uploader_test_snapshot_t snap;
static int fault,cleanup_count,open_flags;
static struct smb2_context ctx;
static struct smb2fh fh;
static struct addrinfo address;
esp_err_t uploader_load_config(uploader_config_t *c){memset(c,0,sizeof(*c));strcpy(c->smb_host,"nas.invalid");strcpy(c->smb_share,"test");strcpy(c->smb_path,"folder");return ESP_OK;}
void uploader_test_stage(uploader_test_stage_t s,bool complete,const char *d){snap.stage=s;if(complete)snap.completed_mask|=1U<<s;snprintf(snap.detail,sizeof(snap.detail),"%s",d);}
void uploader_test_failed(uploader_test_stage_t s,const char *d){snap.failed_mask|=1U<<s;uploader_test_stage(s,false,d);}
uint32_t esp_random(void){return 42;}
int getaddrinfo(const char *h,const char *s,const struct addrinfo *hint,struct addrinfo **out){(void)h;(void)s;(void)hint;*out=&address;return fault==1?-1:0;}
void freeaddrinfo(struct addrinfo *a){(void)a;}
struct smb2_context *smb2_init_context(void){return &ctx;}
void smb2_destroy_context(struct smb2_context *c){(void)c;}
void smb2_set_timeout(struct smb2_context *c,int n){(void)c;assert(n==5);}
void smb2_set_security_mode(struct smb2_context *c,int n){(void)c;(void)n;}
void smb2_set_user(struct smb2_context *c,const char *s){(void)c;(void)s;}
void smb2_set_password(struct smb2_context *c,const char *s){(void)c;(void)s;}
int smb2_connect_share(struct smb2_context *c,const char *h,const char *s,const char *u){(void)c;(void)h;(void)s;(void)u;assert(!(snap.completed_mask&(1U<<UPLOAD_STAGE_CONNECT)));return fault==2?-1:0;}
int smb2_disconnect_share(struct smb2_context *c){(void)c;return 0;}
struct smb2fh *smb2_open(struct smb2_context *c,const char *p,int f){(void)c;assert(strstr(p,"folder/.somnotrace-test-")==p);open_flags=f;return fault==3?NULL:&fh;}
int smb2_pwrite(struct smb2_context *c,struct smb2fh *h,const uint8_t *p,uint32_t n,uint64_t o){(void)c;(void)h;(void)p;(void)o;return fault==4?-1:(int)n;}
int smb2_pread(struct smb2_context *c,struct smb2fh *h,uint8_t *p,uint32_t n,uint64_t o){(void)c;(void)h;(void)o;memset(p,fault==6?0:0x5a,n);return n;}
int smb2_fsync(struct smb2_context *c,struct smb2fh *h){(void)c;(void)h;return fault==5?-1:0;}
int smb2_close(struct smb2_context *c,struct smb2fh *h){(void)c;(void)h;return fault==7?-1:0;}
int smb2_unlink(struct smb2_context *c,const char *p){(void)c;(void)p;cleanup_count++;return fault==8?-1:0;}
int main(void){
 for(fault=0;fault<=8;fault++){
  memset(&snap,0,sizeof(snap));cleanup_count=0;open_flags=0;
  int result=uploader_smb_probe();assert((result==ESP_OK)==(fault==0));
  if(!fault){assert(snap.completed_mask==63&&!snap.failed_mask);assert(open_flags&O_EXCL);}
  if(fault==1)assert(snap.failed_mask==(1U<<UPLOAD_STAGE_RESOLVE));
  if(fault==2){assert(snap.failed_mask==(1U<<UPLOAD_STAGE_AUTH_MOUNT));assert(!(snap.completed_mask&(1U<<UPLOAD_STAGE_CONNECT)));}
  if(fault>=3&&fault<=5)assert(snap.failed_mask==(1U<<UPLOAD_STAGE_WRITE));
  if(fault==6)assert(snap.failed_mask==(1U<<UPLOAD_STAGE_VERIFY));
  if(fault>=7)assert(snap.failed_mask==(1U<<UPLOAD_STAGE_CLEANUP));
  if(fault>=4&&fault<=7){assert(cleanup_count==1);assert(snap.completed_mask&(1U<<UPLOAD_STAGE_CLEANUP));assert(snap.failed_mask);}
 }
 puts("SMB probe injected resolve/auth/create/write/flush/readback/close/cleanup transitions passed");
}
