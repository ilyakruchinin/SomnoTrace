/* Host execution of production validators and the NVS journal. The fake NVS
 * models persistence and write failure, not successful network delivery. */
#include "net_provision.h"
#include "config_redaction.h"
#include "therapy_alert.h"
#include "alert_history.h"
#include "nvs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static time_t fake_now=1800000000;
time_t config_test_time(time_t *out){if(out)*out=fake_now;return fake_now;}
static struct { char key[20]; unsigned char data[64]; size_t len; } records[520];
static int count;static bool fail,commit_fail;
static int find(const char *key){for(int i=0;i<count;i++)if(!strcmp(key,records[i].key))return i;return -1;}
esp_err_t nvs_open(const char *n,int m,nvs_handle_t *h){(void)n;(void)m;*h=1;return fail?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;return fail||commit_fail?ESP_FAIL:ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key){(void)h;int i=find(key);if(i<0)return ESP_ERR_NVS_NOT_FOUND;records[i].len=0;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *v,size_t len){(void)h;if(fail)return ESP_FAIL;int i=find(key);if(i<0){i=count++;assert(count<=520);strcpy(records[i].key,key);}assert(len<=64);memcpy(records[i].data,v,len);records[i].len=len;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *v,size_t *len){(void)h;int i=find(key);if(i<0)return ESP_ERR_NVS_NOT_FOUND;if(*len<records[i].len)return ESP_ERR_INVALID_SIZE;*len=records[i].len;memcpy(v,records[i].data,*len);return ESP_OK;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *v){size_t len=4;return nvs_get_blob(h,key,v,&len);}
esp_err_t nvs_set_u32(nvs_handle_t h,const char *key,uint32_t v){return nvs_set_blob(h,key,&v,4);}
/* The crypto primitive is exercised by firmware. This deterministic stub only
 * supports testing receipt binding/invalidation independently of crypto. */
int mbedtls_sha256(const unsigned char *s,size_t n,unsigned char out[32],int mode){(void)mode;memset(out,0,32);for(size_t i=0;i<n;i++)out[i%32]^=s[i];return 0;}
static void networks(void){
 struct netprov_config cfg={0};strcpy(cfg.wifi[0].ssid,"Primary");strcpy(cfg.wifi[0].pass,"correct-pass");
 assert(netprov_validate_config(&cfg)==ESP_OK);
 strcpy(cfg.wifi[1].ssid,"Primary");assert(netprov_validate_config(&cfg)==ESP_ERR_INVALID_ARG);cfg.wifi[1].ssid[0]=0;
 struct netprov_ipv4 *ip=&cfg.wifi[0].ipv4;ip->manual=true;strcpy(ip->address,"192.168.1.42");strcpy(ip->netmask,"255.255.255.0");strcpy(ip->gateway,"192.168.1.1");strcpy(ip->dns,"192.168.1.1");assert(netprov_validate_config(&cfg)==ESP_OK);
 strcpy(ip->gateway,"192.168.2.1");assert(netprov_validate_config(&cfg)==ESP_ERR_INVALID_ARG);
 strcpy(ip->gateway,"192.168.1.1");strcpy(ip->netmask,"255.0.255.0");assert(netprov_validate_config(&cfg)==ESP_ERR_INVALID_ARG);
 strcpy(ip->netmask,"255.255.255.0");strcpy(ip->address,"192.168.1.255");assert(netprov_validate_config(&cfg)==ESP_ERR_INVALID_ARG);
 strcpy(ip->address,"192.168.1.256");assert(netprov_validate_config(&cfg)==ESP_ERR_INVALID_ARG);
 ip->manual=false;assert(netprov_validate_config(&cfg)==ESP_OK); /* staged addresses do not activate static */
}
static void alert_settings(void){therapy_alert_config_t cfg=ALERT_DEFAULTS;assert(therapy_alert_validate_config(&cfg)==ESP_OK);cfg.win_end=1440;assert(therapy_alert_validate_config(&cfg)==ESP_ERR_INVALID_ARG);cfg.win_end=360;cfg.delay1=181;assert(therapy_alert_validate_config(&cfg)==ESP_ERR_INVALID_ARG);cfg.delay1=5;strcpy(cfg.ntfy_topic,"topic/escape");assert(therapy_alert_validate_config(&cfg)==ESP_ERR_INVALID_ARG);strcpy(cfg.ntfy_topic,"random-topic_0123");strcpy(cfg.ntfy_srv,"https://user:secret@server");assert(therapy_alert_validate_config(&cfg)==ESP_ERR_INVALID_ARG);}
static void journal(void){
 alert_history_page_t page;
 uint32_t cancelled=alert_history_begin(false);alert_history_cancel(cancelled);alert_history_read(0,&page);assert(page.rows[0].result==ALERT_DELIVERY_CANCELLED&&!page.rows[0].acknowledged);
 /* Explicit ack while a send is in flight must survive its later outcome. */
 uint32_t prior=alert_history_begin(false);alert_history_ack(prior);alert_history_result(prior,ALERT_DELIVERY_ACCEPTED,false);
 uint32_t next=alert_history_begin(false);alert_history_cancel(next);alert_history_result(prior,ALERT_DELIVERY_FAILED,true);
 alert_history_read(0,&page);assert(page.count==3);assert(page.rows[0].id==next&&!page.rows[0].acknowledged);assert(page.rows[1].id==prior&&page.rows[1].acknowledged&&page.rows[1].result==ALERT_DELIVERY_FAILED);
 /* Cancellation after service acceptance never invents acknowledgement or
  * rewrites the observed network outcome. */
 uint32_t accepted=alert_history_begin(false);alert_history_result(accepted,ALERT_DELIVERY_ACCEPTED,false);alert_history_cancel(accepted);alert_history_read(0,&page);assert(page.rows[0].result==ALERT_DELIVERY_ACCEPTED&&!page.rows[0].acknowledged);
 alert_history_verify("https://ntfy.example","topic-a",true);assert(alert_history_verified_at("https://ntfy.example","topic-a")==fake_now);assert(!alert_history_verified_at("https://ntfy.example","topic-b"));alert_history_verify("https://ntfy.example","topic-a",false);assert(!alert_history_verified_at("https://ntfy.example","topic-a"));
 /* Storage has no process cache: a fresh read recovers prior outcomes. */
 fake_now+=31*86400;alert_history_read(0,&page);assert(page.count==0);
 for(int i=0;i<ALERT_HISTORY_CAPACITY+3;i++)assert(alert_history_begin(true));alert_history_read(0,&page);assert(page.total==ALERT_HISTORY_CAPACITY&&page.count==ALERT_HISTORY_PAGE);
 uint32_t newest=page.rows[0].id;alert_history_ack(1);alert_history_read(0,&page);assert(page.rows[0].id==newest&&!page.rows[0].acknowledged);
 fail=true;assert(!alert_history_begin(true));alert_history_read(0,&page);assert(page.storage_result==ESP_FAIL);fail=false;
 commit_fail=true;assert(!alert_history_begin(true));commit_fail=false;
 alert_history_read(0,&page);assert(page.storage_result==ESP_OK&&page.write_error==ESP_FAIL);
 assert(alert_history_storage_error()==ESP_FAIL); /* a successful read cannot hide a failed commit */
 fake_now=0;alert_history_begin(true);alert_history_read(0,&page);assert(!page.time_valid&&!page.rows[0].epoch);
}
int main(void){
 char secret[128];memset(secret,'X',sizeof(secret));secret[127]=0;
 config_redact(secret,sizeof(secret),"Saved","Not set");assert(!strcmp(secret,"Saved"));
 for(size_t i=6;i<sizeof(secret);i++)assert(secret[i]==0);
 config_redact(secret,1,"Saved","Not set");assert(secret[0]==0);
 networks();alert_settings();journal();puts("Configuration validation and durable alert lifecycle tests passed");}
