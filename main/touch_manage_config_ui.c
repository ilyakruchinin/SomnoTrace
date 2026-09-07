#include "touch_manage_config.h"
#include "somnotrace_fonts.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BASE 0x181c29
#define PANEL 0x101421
#define CONTROL 0x2d333f
#define TEXT 0xf0f2f6
#define DIM 0xa0a5af
#define LIVE 0x00e1e2
#define WARN 0xf8bd40
#define BAD 0xf45249
#define BODY (&somnotrace_space_grotesk_medium_17)
#define SMALL (&somnotrace_space_grotesk_medium_13)
#define MONO (&somnotrace_ibm_plex_mono_medium_15)
#define TITLE (&somnotrace_space_grotesk_semibold_23)

typedef enum { V_WIFI, V_WIFI_EDIT, V_IP, V_SCAN, V_TIME, V_ZONE,
    V_ALERTS, V_TOPIC, V_HISTORY, V_UPLOADS, V_SMB, V_SHQ, V_FTP,
    V_TEST, V_EDITOR, V_CONFIRM, V_TOPIC_SHOW } view_t;
typedef struct {
    lv_obj_t *root, *field, *keyboard, *preview, *status;
    lv_obj_t *time_value, *sync_value, *wifi_info, *upload_status, *backend_status[2];
    lv_timer_t *timer;
    manage_config_snapshot_t live;
    view_t view, return_view;
    int slot, history_offset;
    bool dirty, was_busy, secret;
    manage_config_command_t command;
    char caption[64], value[128], context[256], backend[12], subtitle[192];
    int drag_slot, drag_y;
    uint32_t drawn_test_state, drawn_scan_state, drawn_test_mask;
    uint32_t result_until;
} ui_t;
static ui_t *s_ui;
static void render(void);
static void event(lv_event_t *e);
static void set_dynamic(lv_obj_t *object, const char *value)
{
    if (object && strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}
static void refresh_dynamic(void)
{
    ui_t *u = s_ui;
    char text[256];
    set_dynamic(u->time_value, u->live.local_time);
    set_dynamic(u->sync_value, u->live.sync_detail);
    if (u->wifi_info) {
        snprintf(text, sizeof(text), "Active: %s  |  IP %s\n%s.local  |  MAC %s",
                 u->live.link.up ? u->live.link.ssid : "Disconnected",
                 u->live.link.up ? u->live.link.ip : "unavailable", u->live.hostname,
                 u->live.mac[0] ? u->live.mac : "unavailable");
        set_dynamic(u->wifi_info, text);
    }
    if (u->upload_status) {
        snprintf(text, sizeof(text), "%s · next scan %lu s", u->live.progress.status,
                 (unsigned long)u->live.progress.next_scan_s);
        set_dynamic(u->upload_status, u->live.progress_valid ? text : "No scheduler observation yet");
        for (size_t i = 0; i < 2; ++i) {
            if (!u->live.progress_valid || i >= u->live.progress.backend_count) {
                set_dynamic(u->backend_status[i], "");
                continue;
            }
            const uploader_backend_progress_t *p = &u->live.progress.backends[i];
            snprintf(text, sizeof(text), "%s: %s · %d nights queued%s", p->label,
                     p->state == UPLOADER_BACKEND_COOLDOWN ? "Backed off" :
                     p->state == UPLOADER_BACKEND_UPLOADING ? "Uploading" :
                     p->state == UPLOADER_BACKEND_DISABLED ? "Disabled" : "Idle",
                     p->days_total - p->days_done, p->error_valid ? " · failure recorded" : "");
            set_dynamic(u->backend_status[i], text);
        }
    }
    if (u->result_until && (int32_t)(lv_tick_get() - u->result_until) >= 0) {
        u->result_until = 0;
        if (u->view != V_TEST && u->view != V_EDITOR && u->view != V_CONFIRM) {
            snprintf(text, sizeof(text), "%s%s", u->live.simulated ? "QEMU SIMULATION · " : "", u->subtitle);
            set_dynamic(u->status, text);
        }
    }
}
static void request(manage_config_command_t kind, const char *value, int index)
{
    manage_config_request_t q = { .kind = kind, .slot = s_ui->slot, .index = index };
    strlcpy(q.value, value ? value : "", sizeof(q.value));
    esp_err_t r = touch_manage_config_submit(&q);
    if (r != ESP_OK) strlcpy(s_ui->live.result, "Another operation is active; try again when it finishes", sizeof(s_ui->live.result));
    else s_ui->was_busy = true;
    memset(&q, 0, sizeof(q)); s_ui->dirty = true;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent); lv_obj_set_pos(l, x, y); lv_obj_set_width(l, width);
    lv_label_set_text(l, text); lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0); return l;
}
static lv_obj_t *button(const char *text, int x, int y, int width, int height, int action)
{
    lv_obj_t *b = lv_btn_create(s_ui->root); lv_obj_set_pos(b,x,y==0?8:y); lv_obj_set_size(b,width,height);
    lv_obj_set_style_bg_color(b,lv_color_hex(CONTROL),0); lv_obj_set_style_radius(b,height>=44?22:12,0);
    lv_obj_set_style_border_width(b,0,0); lv_obj_set_style_shadow_width(b,0,0); lv_obj_set_style_pad_all(b,0,0);
    lv_obj_t *l=label(b,text,12,12,width-24,BODY,TEXT);
    if (height == 44) lv_obj_set_y(l,11);
    lv_obj_add_event_cb(b,event,LV_EVENT_PRESSED,(void *)(intptr_t)action); return b;
}
/* Local actions are separate from typed service commands. */
enum { A_BACK=1000,A_TIME,A_NETWORKS,A_SLOT0,A_SLOT1,A_SLOT2,A_SLOT3,A_IP,
       A_SCAN,A_ZONE,A_TOPIC,A_HISTORY,A_SMB,A_SHQ,A_FTP,A_TEST,A_RETRY,
       A_RETRY_ALL,A_SAVE,A_CANCEL,A_SHOW,A_CONFIRM,A_GENERATE,A_FORGET,
       A_RECONNECT,A_PRIMARY,A_HISTORY_PREV,A_HISTORY_NEXT,A_ZONE0,A_ZONE1,
       A_ZONE2,A_ZONE3,A_ZONE4,A_ZONE5,A_SIM_FAIL,A_TOPIC_SHOW,A_OPEN_NETWORK };
static void title(const char *text, const char *sub)
{
    label(s_ui->root,text,12,6,500,TITLE,TEXT);
    strlcpy(s_ui->subtitle,sub,sizeof(s_ui->subtitle));
    s_ui->status=label(s_ui->root,s_ui->subtitle,12,54,740,SMALL,DIM);
    lv_label_set_long_mode(s_ui->status,LV_LABEL_LONG_DOT); lv_obj_set_height(s_ui->status,17);
}
static void row(const char *name,const char *value,int y,int action,bool mono)
{
    lv_obj_t *b=button("",12,y,740,44,action);
    lv_obj_set_style_bg_color(b,lv_color_hex(PANEL),0); lv_obj_set_style_radius(b,12,0);
    label(b,name,12,10,340,BODY,TEXT);
    lv_obj_t *l=label(b,value,356,11,364,mono?MONO:BODY,DIM);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_RIGHT,0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_obj_set_height(l, 24);
}
static void field(const char *name,const char *value,int y,manage_config_command_t cmd)
{ row(name,value,y,cmd,true); }
static void format_date(uint32_t epoch,char *out,size_t size)
{
    if (!epoch) { strlcpy(out,"Time unavailable",size); return; }
    time_t time=epoch; struct tm tm; localtime_r(&time,&tm); strftime(out,size,"%d %b %H:%M",&tm);
}
static void preview_path(const char *edited)
{
    char out[320]; const uploader_config_t *u=&s_ui->live.uploads;
    const char *path=edited?edited:u->smb_path;
    while (*path=='/' || *path=='\\') path++;
    snprintf(out,sizeof(out),"\\\\%s\\%s%s%s",u->smb_host,u->smb_share,*path?"\\":"",path);
    for (char *p=out; *p; p++) if (*p=='/') *p='\\';
    if (s_ui->preview) lv_label_set_text(s_ui->preview,out);
}
/* Text-only keycaps use glyphs in the licensed native font. The controller
 * handles every command explicitly; no missing icon glyph doubles as a key. */
static const char *config_keys_lower[] = { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "Del", "\n", "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n", "Shift", "z", "x", "c", "v", "b", "n", "m", ".", "/", "\n", "123", "@", "_", "Space", "Done", "" };
static const lv_btnmatrix_ctrl_t config_ctrl_lower[] = { 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 2 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 3 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 2 | LV_BTNMATRIX_CTRL_CLICK_TRIG };
static const char *config_keys_upper[] = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Del", "\n", "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n", "Shift", "Z", "X", "C", "V", "B", "N", "M", ".", "/", "\n", "123", "@", "_", "Space", "Done", "" };
static const lv_btnmatrix_ctrl_t config_ctrl_upper[] = { 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 2 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 3 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 2 | LV_BTNMATRIX_CTRL_CLICK_TRIG };
static const char *config_keys_symbols[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Del", "\n", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "\n", "+", "-", "=", ":", "/", ";", ",", ".", "?", "\\", "abc", "\n", "<", ">", "[", "]", "{", "}", "\"", "'", "`", "~", "|", "Space", "Done", "" };
static const lv_btnmatrix_ctrl_t config_ctrl_symbols[] = { 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG, 1 | LV_BTNMATRIX_CTRL_CLICK_TRIG };
static void keyboard_input(lv_event_t *e)
{
    if (!s_ui || !s_ui->field) return;
    lv_obj_t *kb=lv_event_get_target(e);
    uint16_t id=lv_btnmatrix_get_selected_btn(kb);
    if (id==LV_BTNMATRIX_BTN_NONE) return;
    const char *key=lv_btnmatrix_get_btn_text(kb,id);
    if (!key) return;
    if (!strcmp(key,"Done")) { lv_event_send(kb,LV_EVENT_READY,NULL); return; }
    if (!strcmp(key,"Del")) { lv_textarea_del_char(s_ui->field); return; }
    if (!strcmp(key,"Shift")) { lv_keyboard_set_mode(kb,lv_keyboard_get_mode(kb)==LV_KEYBOARD_MODE_TEXT_LOWER?LV_KEYBOARD_MODE_TEXT_UPPER:LV_KEYBOARD_MODE_TEXT_LOWER); return; }
    if (!strcmp(key,"123")) { lv_keyboard_set_mode(kb,LV_KEYBOARD_MODE_SPECIAL); return; }
    if (!strcmp(key,"abc")) { lv_keyboard_set_mode(kb,LV_KEYBOARD_MODE_TEXT_LOWER); return; }
    lv_textarea_add_text(s_ui->field,!strcmp(key,"Space")?" ":key);
}
static void keyboard_event(lv_event_t *e)
{
    if (!s_ui) return;
    if (lv_event_get_code(e)==LV_EVENT_READY) {
        strlcpy(s_ui->value,lv_textarea_get_text(s_ui->field),sizeof(s_ui->value));
        if (s_ui->command==MC_TIMEZONE_SEARCH) { request(MC_TIMEZONE_SEARCH,s_ui->value,0); s_ui->view=V_ZONE; }
        else { request(s_ui->command,s_ui->value,0); s_ui->view=s_ui->return_view; }
        memset(s_ui->value,0,sizeof(s_ui->value)); s_ui->dirty=true;
    } else if (lv_event_get_code(e)==LV_EVENT_CANCEL) { s_ui->view=s_ui->return_view; s_ui->dirty=true; }
}
static void field_changed(lv_event_t *e)
{ if (s_ui && s_ui->command==MC_SMB_PATH) preview_path(lv_textarea_get_text(lv_event_get_target(e))); }
static void editor(manage_config_command_t command,const char *caption,const char *value,bool secret,const char *context)
{
    s_ui->return_view=s_ui->view; s_ui->view=V_EDITOR; s_ui->command=command; s_ui->secret=secret;
    strlcpy(s_ui->caption,caption,sizeof(s_ui->caption));
    strlcpy(s_ui->value,secret?"":value,sizeof(s_ui->value));
    strlcpy(s_ui->context,context?context:"",sizeof(s_ui->context)); s_ui->dirty=true;
}
static void confirm(manage_config_command_t command,const char *caption,const char *context)
{
    s_ui->return_view=s_ui->view; s_ui->view=V_CONFIRM; s_ui->command=command;
    strlcpy(s_ui->caption,caption,sizeof(s_ui->caption)); strlcpy(s_ui->context,context,sizeof(s_ui->context)); s_ui->dirty=true;
}
static void drag(lv_event_t *e)
{
    if (!s_ui || s_ui->view!=V_WIFI) return;
    lv_indev_t *input=lv_indev_get_act(); if (!input) return;
    lv_point_t p; lv_indev_get_point(input,&p);
    int slot=(int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e)==LV_EVENT_PRESSED) { s_ui->drag_slot=slot; s_ui->drag_y=p.y; }
    if (lv_event_get_code(e)==LV_EVENT_RELEASED) {
        int delta=p.y-s_ui->drag_y;
        if (abs(delta)>28) {
            int to=slot+(delta>0?(delta+29)/58:(delta-29)/58); if (to<0) to=0; if (to>3) to=3;
            s_ui->slot=slot; request(MC_WIFI_MOVE,"",to);
        } else { s_ui->slot=slot; s_ui->view=V_WIFI_EDIT; s_ui->dirty=true; }
    }
}
static void render_wifi(void)
{
    title("Wi-Fi networks","2.4 GHz only. Slot order is fallback priority; saving does not disconnect.");
    button("Time",628,0,124,44,A_TIME); button("Scan",510,0,108,44,A_SCAN);
    for(int i=0;i<4;i++) {
        const struct netprov_wifi_cred *w=&s_ui->live.wifi.wifi[i]; char sub[120];
        const char *state="range unknown";
        bool connected=s_ui->live.link.up&&!strcmp(w->ssid,s_ui->live.link.ssid);
        if(s_ui->live.scan.state==NETPROV_SCAN_READY) {
            state="not seen in latest scan";
            for(size_t j=0;j<s_ui->live.scan.count;j++) if(!strcmp(w->ssid,s_ui->live.scan.aps[j].ssid)) state="seen in latest scan";
        }
        snprintf(sub,sizeof(sub),"Slot %d · %s · %s",i+1,i?"fallback":"primary",connected?"Connected":state);
        lv_obj_t *b=button("",12,76+i*58,740,54,A_SLOT0+i);
        lv_obj_set_style_bg_color(b,lv_color_hex(PANEL),0); lv_obj_set_style_radius(b,12,0);
        lv_obj_remove_event_cb(b,event); lv_obj_add_flag(b, LV_OBJ_FLAG_PRESS_LOCK); lv_obj_add_event_cb(b,drag,LV_EVENT_ALL,(void *)(intptr_t)i);
        label(b,w->ssid[0]?w->ssid:"Add network",12,4,450,BODY,w->ssid[0]?TEXT:DIM);
        label(b,w->ssid[0]?sub:"Empty saved slot",12,29,690,SMALL,DIM);
        if(connected&&s_ui->live.link.rssi_valid) { char r[24];snprintf(r,sizeof(r),"%d dBm",s_ui->live.link.rssi);label(b,r,560,7,154,MONO,LIVE); }
    }
    char info[256];snprintf(info,sizeof(info),"Active: %s  |  IP %s\n%s.local  |  MAC %s",s_ui->live.link.up?s_ui->live.link.ssid:"Disconnected",s_ui->live.link.up?s_ui->live.link.ip:"unavailable",s_ui->live.hostname,s_ui->live.mac[0]?s_ui->live.mac:"unavailable");
    s_ui->wifi_info=label(s_ui->root,info,24,319,712,MONO,DIM);
    label(s_ui->root,"Drag to reorder. Tap to edit, forget, or move to primary.",24,377,712,SMALL,DIM);
    button("Reconnect now",536,399,216,44,A_RECONNECT);
}
static const uploader_backend_progress_t *backend_progress(const char *id)
{
    for (size_t i=0;i<s_ui->live.progress.backend_count;i++)
        if (!strcmp(s_ui->live.progress.backends[i].id,id)) return &s_ui->live.progress.backends[i];
    return NULL;
}
static void destination_receipt(const char *id,int y)
{
    const uploader_backend_progress_t *p=backend_progress(id);
    if (!p) { label(s_ui->root,"No scheduler observation yet",24,y,710,SMALL,DIM); return; }
    char date[32], text[160];
    format_date(p->last_success_valid?p->last_success_epoch_s:0,date,sizeof(date));
    snprintf(text,sizeof(text),"Last upload: %s%s",p->last_success_valid?date:"No successful upload recorded",p->state==UPLOADER_BACKEND_COOLDOWN?" · backed off":"");
    label(s_ui->root,text,24,y,710,SMALL,p->error_valid?WARN:DIM);
    if (p->error_valid) {
        snprintf(text,sizeof(text),"%s · retry in %lu s · %d nights queued",p->error,(unsigned long)(p->retry_valid?p->retry_in_s:0),p->days_total-p->days_done);
        label(s_ui->root,text,24,y+22,710,SMALL,WARN);
    }
}
static void render(void)
{
    ui_t *u=s_ui; if(!u) return;
    lv_obj_clean(u->root); u->field=u->keyboard=u->preview=u->status=NULL;
    u->time_value=u->sync_value=u->wifi_info=u->upload_status=NULL;
    memset(u->backend_status, 0, sizeof(u->backend_status));
    if (!u->live.ready) { title("Loading configuration", "Reading saved settings on the service worker…"); u->dirty=false; return; }
    char value[160];
    const struct netprov_wifi_cred *w=&u->live.wifi.wifi[u->slot];
    const therapy_alert_config_t *a=&u->live.alerts; const uploader_config_t *up=&u->live.uploads;
    switch(u->view) {
    case V_WIFI: render_wifi();
        break;
    case V_WIFI_EDIT:
        title(w->ssid[0]?w->ssid:"Add Wi-Fi network","Changes apply on the next reconnect. Recording blocks reconnects.");
        button("Back",640,0,112,44,A_BACK);
        field("Network name",w->ssid,82,MC_WIFI_SSID); field("Password",w->pass,132,MC_WIFI_PASSWORD);
        row("Addressing",w->ipv4.manual?"Static IPv4":"DHCP (automatic)",182,A_IP,false);
        row("Move to primary","Save priority without reconnecting",232,A_PRIMARY,false);
        row("Forget network","Confirmation required",282,A_FORGET,false);
        label(u->root,"To replace a password, enter the new value. Blank keeps it.\nForget and re-add to clear a saved password for an open network.",24,344,710,SMALL,DIM);
        break;
    case V_IP:
        title("IPv4 addressing","Fill all four addresses before enabling static. Changes apply at reconnect."); button("Back",640,0,112,44,A_BACK);
        field("IP address",w->ipv4.address,82,MC_IP_ADDRESS); field("Subnet mask",w->ipv4.netmask,132,MC_IP_MASK);
        field("Gateway",w->ipv4.gateway,182,MC_IP_GATEWAY); field("DNS server",w->ipv4.dns,232,MC_IP_DNS);
        row("Addressing mode",w->ipv4.manual?"Static enabled · tap for DHCP":"DHCP · tap to enable static",282,MC_IP_MODE,false);
        break;
    case V_SCAN:
        title("Nearby networks","Results are observed at scan time. Hidden networks can be entered in any empty slot.");button("Back",640,0,112,44,A_BACK);button("Scan",518,0,112,44,MC_WIFI_SCAN);
        for(size_t i=0;i<u->live.scan.count&&i<6;i++) { snprintf(value,sizeof(value),"%d dBm · %s",u->live.scan.aps[i].rssi,u->live.scan.aps[i].secure?"secured":"open");row(u->live.scan.aps[i].ssid,value,82+i*50,MC_REFRESH,true); }
        if(!u->live.scan.count) label(u->root,u->live.scan.state==NETPROV_SCAN_RUNNING?"Scanning…":"No scan results. Scans are blocked during recording.",24,106,680,BODY,DIM);
        break;
    case V_TIME:
        title("Time","Nights use local time. A new zone does not re-date existing recordings.");button("Wi-Fi",640,0,112,44,A_NETWORKS);button("Sync",518,0,112,44,MC_TIME_SYNC);
        u->time_value=label(u->root,u->live.local_time,24,85,710,TITLE,TEXT);
        if(!u->live.timezone[0]) label(u->root,"UTC: no time zone set; nights may be filed under the wrong date.",24,125,710,SMALL,WARN);
        row("Time zone",u->live.timezone[0]?u->live.timezone:"Not set · UTC",164,A_ZONE,true);
        field("NTP server",u->live.ntp[0]?u->live.ntp:"Auto / DHCP + public pools",214,MC_NTP);
        field("Hostname (.local)",u->live.hostname,264,MC_HOSTNAME);
        u->sync_value=label(u->root,u->live.sync_detail,24,330,710,MONO,DIM);
        label(u->root,"NTP callback does not identify the responding server or clock offset.",24,383,710,SMALL,DIM);
        break;
    case V_ZONE:
        title("Time zone catalogue","Search an IANA city or region; displayed offsets are catalogue metadata.");button("Back",640,0,112,44,A_BACK);button("Search",510,0,120,44,A_ZONE);
        for(size_t i=0;i<u->live.zone_count;i++) { snprintf(value,sizeof(value),"%s · %s",u->live.zones[i].utc_offset,u->live.zones[i].abbreviation);row(u->live.zones[i].id,value,82+i*50,A_ZONE0+i,true); }
        if(!u->live.zone_count) label(u->root,"Search for a city, for example Toronto or London.",24,110,680,BODY,DIM);
        break;
    case V_ALERTS:
        title("Therapy interruption alerts",!a->enabled?"Disabled":!a->push_en||!a->ntfy_topic[0]?"Screen only · partial delivery; no speaker on this board":u->live.verified_at?"Push service accepted a prior test/send; phone delivery is unverified":"Push configured, acceptance unverified. Screen alerts still work.");
        button("History",532,0,106,44,A_HISTORY);button("Test",648,0,104,44,A_TEST);
        row("Interruption alerts",a->enabled?"Enabled":"Disabled",76,MC_ALERT_ENABLE,false);
        label(u->root,"WHEN",24,122,710,SMALL,DIM);
        snprintf(value,sizeof(value),"%02u:%02u-%02u:%02u",a->win_start/60,a->win_start%60,a->win_end/60,a->win_end%60);field("Alert window",value,143,MC_ALERT_WINDOW);
        snprintf(value,sizeof(value),"%u min",a->delay1);field("Initial delay after therapy stops",value,189,MC_ALERT_DELAY1);
        snprintf(value,sizeof(value),"%u min",a->delay2);field("Escalation if unacknowledged",value,235,MC_ALERT_DELAY2);
        label(u->root,"HOW · No speaker. Phone settings control sound and silent mode.",24,282,710,SMALL,DIM);
        row("Delivery",a->push_en?"Push and screen":"Screen only (partial)",303,MC_ALERT_PUSH,false);
        row("ntfy server / topic",a->ntfy_topic[0]?"Configured · edit / show":"No topic · set up",349,A_TOPIC,false);
        snprintf(value,sizeof(value),"%u / 5",a->ntfy_prio);field("Priority",value,395,MC_ALERT_PRIORITY);
        break;
    case V_TOPIC:
        title("ntfy push",u->live.history.write_error?"History persistence failed this boot; acceptance receipts may be incomplete.":"A topic is a shared secret: anyone who knows it can read your alerts.");button("Back",640,0,112,44,A_BACK);
        field("Server",a->ntfy_srv,82,MC_ALERT_SERVER);field("Topic",a->ntfy_topic[0]?a->ntfy_topic:"Not set",132,MC_ALERT_TOPIC);
        row("Generate a new topic","Existing phone subscriptions stop working",196,A_GENERATE,false);
        row("Show subscription details","Read / enter on your phone",254,A_TOPIC_SHOW,false);
        format_date(u->live.verified_at,value,sizeof(value));label(u->root,u->live.verified_at?"Last push service acceptance (phone delivery not verified)":"Push channel has no matching acceptance receipt",24,325,700,BODY,u->live.verified_at?LIVE:WARN);
        if(u->live.verified_at)label(u->root,value,24,360,680,MONO,DIM);
        break;
    case V_TOPIC_SHOW:
        title("Subscribe on your phone","This device has no shared clipboard. Read these values into your ntfy app.");button("Back",640,0,112,44,A_BACK);
        label(u->root,"SERVER",24,94,680,SMALL,DIM);label(u->root,a->ntfy_srv,24,124,710,MONO,TEXT);
        label(u->root,"TOPIC · shared secret",24,205,680,SMALL,DIM);label(u->root,a->ntfy_topic,24,239,710,MONO,TEXT);
        break;
    case V_HISTORY:
        title("Recent alerts",u->live.history.write_error?"A history write failed this boot; some outcomes may be missing.":"Last 30 days, up to 512 receipts. Service acceptance does not prove phone delivery.");button("Back",640,0,112,44,A_BACK);
        for(size_t i=0;i<u->live.history.count&&i<6;i++) { const alert_history_record_t *r=&u->live.history.rows[i];char date[32];format_date(r->epoch,date,sizeof(date));
            snprintf(value,sizeof(value),"%s · %s%s",date,r->test?"test":"interruption",r->escalated?" · escalated":"");
            const char *result=r->acknowledged?"Acknowledged on device":r->result==ALERT_DELIVERY_FAILED?"Push failed":r->result==ALERT_DELIVERY_ACCEPTED?(r->test?"Service accepted":"Accepted · no acknowledgement"):r->result==ALERT_DELIVERY_SCREEN?"Screen only":r->result==ALERT_DELIVERY_CANCELLED?"Cancelled / resolved":"Outcome not recorded";
            row(value,result,78+i*49,MC_REFRESH,false);
        }
        if(!u->live.history.count)label(u->root,u->live.history.storage_result!=ESP_OK&&u->live.history.storage_result!=ESP_ERR_NVS_NOT_FOUND?"Alert history unavailable":"No retained alert receipts",24,104,680,BODY,DIM);
        button("Previous",12,398,124,44,A_HISTORY_PREV);button("Next",628,398,124,44,A_HISTORY_NEXT);
        break;
    case V_UPLOADS:
        title("Uploads and local file access","SMB and SleepHQ copy recordings. The card retains its copy.");button("Retry all",612,0,140,44,A_RETRY_ALL);
        row("SMB",up->smb_enabled?(up->smb_host[0]&&up->smb_share[0]?"Enabled · status below":"Enabled · configuration incomplete"):"Disabled",82,A_SMB,false);
        row("SleepHQ",up->shq_enabled?(up->shq_client_id[0]?"Enabled · status below":"Enabled · configuration incomplete"):"Disabled",138,A_SHQ,false);
        row("FTP local file server",up->ftp_enabled?"Enabled at next restart":"Disabled at next restart",194,A_FTP,false);
        snprintf(value,sizeof(value),"%d days",up->max_days);field("Upload history window",value,250,MC_UPLOAD_DAYS);
        u->upload_status=label(u->root,"",24,314,710,MONO,DIM);
        for(size_t i=0;i<2;i++) u->backend_status[i]=label(u->root,"",24,349+i*28,710,SMALL,DIM);
        refresh_dynamic();
        break;
    case V_SMB:
        title("SMB","Test creates a temporary 4 KiB remote file, reads it back and removes it.");button("Back",656,0,96,44,A_BACK);button("Test",550,0,96,44,A_TEST);button("Retry",444,0,96,44,A_RETRY);
        row("Outgoing uploads",up->smb_enabled?"Enabled":"Disabled",78,MC_SMB_ENABLE,false);
        field("Host",up->smb_host,124,MC_SMB_HOST);field("Share",up->smb_share,170,MC_SMB_SHARE);field("Path",up->smb_path,216,MC_SMB_PATH);
        field("Username",up->smb_user,262,MC_SMB_USER);field("Password",up->smb_pass,308,MC_SMB_PASSWORD);
        u->preview=label(u->root,"",24,365,710,MONO,DIM);preview_path(NULL);destination_receipt("smb",399);
        break;
    case V_SHQ:
        title("SleepHQ","Credential test uses OAuth and account lookup; it does not upload recordings.");button("Back",656,0,96,44,A_BACK);button("Test",550,0,96,44,A_TEST);button("Retry",444,0,96,44,A_RETRY);
        row("Outgoing uploads",up->shq_enabled?"Enabled":"Disabled",82,MC_SHQ_ENABLE,false);
        field("Client ID",up->shq_client_id,140,MC_SHQ_ID);field("Client secret",up->shq_client_secret,198,MC_SHQ_SECRET);destination_receipt("sleephq",280);
        break;
    case V_FTP:
        title("FTP local file server","Files are accessed from a computer on your network; FTP is not an upload destination.");button("Back",640,0,112,44,A_BACK);
        row("Enable at next restart",up->ftp_enabled?"Enabled":"Disabled",82,MC_FTP_ENABLE,false);
        row("Authentication",up->ftp_anonymous?"Anonymous":"Username and password",132,MC_FTP_ANON,false);
        field("Username",up->ftp_user,182,MC_FTP_USER);field("Password",up->ftp_pass,232,MC_FTP_PASSWORD);
        snprintf(value,sizeof(value),"ftp://%s:21 · %s",u->live.link.up?u->live.link.ip:"network unavailable",u->live.ftp_running?"service enabled":"service not running");label(u->root,value,24,310,710,MONO,DIM);
        label(u->root,"FTP is unencrypted. Saved changes take effect after a safe restart in System.",24,365,700,SMALL,WARN);
        break;
    case V_TEST: {
        title(!strcmp(u->backend,"smb")?"SMB connection test":"SleepHQ credential test",u->live.test.detail);button("Back",640,0,112,44,A_BACK);
        const char *names[]={"Resolve host","Connect transport","Authenticate / mount or account","Write diagnostic file","Verify readback","Remove diagnostic file"};
        for(int i=0;i<6;i++) { bool relevant=!strcmp(u->backend,"smb")||i==1||i==2;row(names[i],!relevant?"Not part of this test":u->live.test.failed_mask&(1<<i)?"Failed":u->live.test.completed_mask&(1<<i)?"Observed success":u->live.test.stage==i?(u->live.test.state==UPLOAD_TEST_FAILED?"Failed":"Current stage"):"Not observed",82+i*50,MC_REFRESH,false); }
        break;
    }
    case V_EDITOR:
        title(u->caption,u->context);button("Cancel",528,0,106,44,A_CANCEL);button("Save",644,0,108,44,A_SAVE);
        u->field=lv_textarea_create(u->root);lv_obj_set_pos(u->field,20,85);lv_obj_set_size(u->field,728,56);
        lv_textarea_set_one_line(u->field,true);lv_textarea_set_max_length(u->field,127);lv_textarea_set_password_mode(u->field,u->secret);lv_textarea_set_text(u->field,u->value);lv_obj_set_size(u->field,728,56);
        lv_obj_set_style_bg_color(u->field,lv_color_hex(CONTROL),0);lv_obj_set_style_text_color(u->field,lv_color_hex(TEXT),0);lv_obj_set_style_text_font(u->field,MONO,0);lv_obj_set_style_border_color(u->field,lv_color_hex(LIVE),0);
        lv_obj_add_event_cb(u->field,field_changed,LV_EVENT_VALUE_CHANGED,NULL);
        if(u->secret)button("Show",630,148,118,44,A_SHOW);
        if(u->command==MC_SMB_PATH){u->preview=label(u->root,"",24,151,580,MONO,DIM);preview_path(u->value);}
        else label(u->root,u->secret?"Blank keeps the saved secret.":u->command==MC_ALERT_TOPIC?"A topic is shared; anyone who knows it can read alerts.":"The rest of the form returns when editing ends.",24,153,580,SMALL,DIM);
        u->keyboard=lv_keyboard_create(u->root);lv_obj_set_align(u->keyboard,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(u->keyboard,20,207);lv_obj_set_size(u->keyboard,728,232);lv_keyboard_set_textarea(u->keyboard,u->field);
        lv_keyboard_set_map(u->keyboard,LV_KEYBOARD_MODE_TEXT_LOWER,config_keys_lower,config_ctrl_lower);
        lv_keyboard_set_map(u->keyboard,LV_KEYBOARD_MODE_TEXT_UPPER,config_keys_upper,config_ctrl_upper);
        lv_keyboard_set_map(u->keyboard,LV_KEYBOARD_MODE_SPECIAL,config_keys_symbols,config_ctrl_symbols);
        lv_obj_remove_event_cb(u->keyboard,lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(u->keyboard,keyboard_input,LV_EVENT_VALUE_CHANGED,NULL);
        lv_obj_set_style_bg_opa(u->keyboard,LV_OPA_TRANSP,0);lv_obj_set_style_pad_row(u->keyboard,7,0);lv_obj_set_style_pad_column(u->keyboard,6,0);
        lv_obj_set_style_bg_color(u->keyboard,lv_color_hex(CONTROL),LV_PART_ITEMS);lv_obj_set_style_text_color(u->keyboard,lv_color_hex(TEXT),LV_PART_ITEMS);lv_obj_set_style_text_font(u->keyboard,BODY,LV_PART_ITEMS);lv_obj_set_style_radius(u->keyboard,12,LV_PART_ITEMS);
        lv_obj_add_event_cb(u->keyboard,keyboard_event,LV_EVENT_READY,NULL);lv_obj_add_event_cb(u->keyboard,keyboard_event,LV_EVENT_CANCEL,NULL);
        break;
    case V_CONFIRM:
        title(u->caption,"Confirmation required");label(u->root,u->context,24,110,710,TITLE,WARN);
        button("Cancel",24,302,200,56,A_CANCEL);button("Confirm",526,302,226,56,A_CONFIRM);
        if(u->live.simulated&&(u->command==MC_ALERT_TEST||u->command==MC_UPLOAD_TEST))button("Simulate failure",484,376,268,56,A_SIM_FAIL);
        break;
    }
    if(u->live.simulated && u->status && !u->live.result[0]) {
        char subtitle[224]; snprintf(subtitle,sizeof(subtitle),"QEMU SIMULATION · %s",u->subtitle); lv_label_set_text(u->status,subtitle);
    }
    if(u->live.result[0] && lv_tick_get() < u->result_until && u->view!=V_TEST && u->view!=V_EDITOR && u->view!=V_CONFIRM && u->status) lv_label_set_text(u->status,u->live.result);
    u->dirty=false;u->drawn_test_state=u->live.test.state;u->drawn_scan_state=u->live.scan.state;u->drawn_test_mask=u->live.test.completed_mask | (u->live.test.failed_mask<<8);
}
static void event(lv_event_t *e)
{
    ui_t *u=s_ui;if(!u)return;
    int action=(int)(intptr_t)lv_event_get_user_data(e);
    const struct netprov_wifi_cred *w=&u->live.wifi.wifi[u->slot];
    const therapy_alert_config_t *a=&u->live.alerts;const uploader_config_t *p=&u->live.uploads;
    char value[128];u->live.result[0]=0;
    switch(action) {
    case A_BACK:
        u->view=u->view==V_WIFI_EDIT||u->view==V_SCAN?V_WIFI:u->view==V_IP?V_WIFI_EDIT:u->view==V_ZONE?V_TIME:u->view==V_TOPIC_SHOW?V_TOPIC:u->view==V_TOPIC||u->view==V_HISTORY?V_ALERTS:u->view==V_TEST?(!strcmp(u->backend,"smb")?V_SMB:V_SHQ):V_UPLOADS;
        break;
    case A_TIME:u->view=V_TIME;
        break;case A_NETWORKS:u->view=V_WIFI;
        break;
    case A_IP:u->view=V_IP;
        break;case A_SCAN:u->view=V_SCAN;request(MC_WIFI_SCAN,"",0);
        break;
    case A_ZONE:editor(MC_TIMEZONE_SEARCH,"Search time zones","",false,"Search the embedded IANA catalogue by city or region.");
        break;
    case A_TOPIC:u->view=V_TOPIC;
        break;case A_TOPIC_SHOW:u->view=V_TOPIC_SHOW;
        break;
    case A_HISTORY:u->view=V_HISTORY;u->history_offset=0;request(MC_ALERT_HISTORY,"",0);
        break;
    case A_HISTORY_PREV:u->history_offset=u->history_offset>=6?u->history_offset-6:0;request(MC_ALERT_HISTORY,"",u->history_offset);
        break;
    case A_HISTORY_NEXT:if(u->history_offset+6<(int)u->live.history.total){u->history_offset+=6;request(MC_ALERT_HISTORY,"",u->history_offset);}break;
    case A_SMB:u->view=V_SMB;strlcpy(u->backend,"smb",12);
        break;
    case A_SHQ:u->view=V_SHQ;strlcpy(u->backend,"sleephq",12);
        break;
    case A_FTP:u->view=V_FTP;
        break;
    case A_TEST:
        if(u->view==V_ALERTS||u->view==V_TOPIC)confirm(MC_ALERT_TEST,"Send one test push?","Sends a real notification to your saved ntfy topic. Phone delivery remains unverified until you check your phone.");
        else confirm(MC_UPLOAD_TEST,"Test this destination?",u->view==V_SMB?"Connects to SMB, writes a temporary 4 KiB file, reads it back and removes it. No patient recording is sent.":"Authenticates with SleepHQ and checks account access. No recording or import is sent.");
        break;
    case A_RETRY:confirm(MC_UPLOAD_RETRY,"Retry this destination?","Queues outstanding recordings for this destination. Other destination receipts and backoff are preserved.");
        break;
    case A_RETRY_ALL:u->backend[0]=0;confirm(MC_UPLOAD_RETRY,"Retry both upload destinations?","Retries pending recordings for enabled destinations. Nothing is deleted from the card.");
        break;
    case A_GENERATE:confirm(MC_ALERT_GENERATE,"Generate a new topic?","Your subscribed phones stop receiving until you enter the new topic in each ntfy app.");
        break;
    case A_FORGET:confirm(MC_WIFI_FORGET,"Forget this network?","Removes the saved credentials. The current link stays connected until its next reconnect.");
        break;
    case A_RECONNECT:confirm(MC_WIFI_RECONNECT,"Reconnect Wi-Fi now?","Disconnects Wi-Fi and tries saved networks in priority order. Blocked during therapy or recording.");
        break;
    case A_PRIMARY:request(MC_WIFI_MOVE,"",0);u->slot=0;
        break;
    case A_CONFIRM:case A_SIM_FAIL: {
        int failed=action==A_SIM_FAIL;
        request(u->command,(u->command==MC_UPLOAD_TEST||u->command==MC_UPLOAD_RETRY)?u->backend:"",failed);
        u->view=u->command==MC_UPLOAD_TEST?V_TEST:u->return_view;
        break;
    }
    case A_CANCEL:u->view=u->return_view;memset(u->value,0,sizeof(u->value));
        break;
    case A_SAVE:
        strlcpy(u->value,lv_textarea_get_text(u->field),sizeof(u->value));
        request(u->command,u->value,0);u->view=u->command==MC_TIMEZONE_SEARCH?V_ZONE:u->return_view;
        memset(u->value,0,sizeof(u->value));
        break;
    case A_SHOW:
        lv_textarea_set_password_mode(u->field,!lv_textarea_get_password_mode(u->field));
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e),0),lv_textarea_get_password_mode(u->field)?"Show":"Hide");
        return;
    case MC_WIFI_SSID:editor(action,"Network name",w->ssid,false,"Case-sensitive SSID. The ESP32-S3 supports 2.4 GHz networks only.");
        break;
    case MC_WIFI_PASSWORD:editor(action,"Network password","",true,"8–63 characters for a secured network. Blank preserves the saved password.");
        break;
    case MC_IP_ADDRESS:editor(action,"Static IP address",w->ipv4.address,false,"Example: 192.168.1.42. Addresses must share a subnet with the gateway.");
        break;
    case MC_IP_MASK:editor(action,"Subnet mask",w->ipv4.netmask,false,"Example: 255.255.255.0");
        break;
    case MC_IP_GATEWAY:editor(action,"Gateway",w->ipv4.gateway,false,"Example: 192.168.1.1");
        break;
    case MC_IP_DNS:editor(action,"DNS server",w->ipv4.dns,false,"IPv4 address of your DNS server.");
        break;
    case MC_NTP:editor(action,"NTP server",u->live.ntp,false,"Hostname or IP, without a URL scheme. Blank restores auto / DHCP. Tap Sync to apply.");
        break;
    case MC_HOSTNAME:editor(action,"Device hostname",u->live.hostname,false,"The device answers as this name plus .local. Letters, digits and hyphens only.");
        break;
    case MC_ALERT_SERVER:editor(action,"ntfy server",a->ntfy_srv,false,"Full http:// or https:// server URL. HTTPS protects the topic in transit.");
        break;
    case MC_ALERT_TOPIC:editor(action,"ntfy topic",a->ntfy_topic,false,"A shared secret; letters, digits, hyphens and underscores only.");
        break;
    case MC_ALERT_WINDOW:snprintf(value,sizeof(value),"%02u:%02u-%02u:%02u",a->win_start/60,a->win_start%60,a->win_end/60,a->win_end%60);editor(action,"Alert window",value,false,"HH:MM-HH:MM in local time. An end before the start crosses midnight.");
        break;
    case MC_ALERT_DELAY1:snprintf(value,sizeof(value),"%u",a->delay1);editor(action,"Initial delay in minutes",value,false,"0–180 minutes. Ignores brief mask-off breaks after therapy stops.");
        break;
    case MC_ALERT_DELAY2:snprintf(value,sizeof(value),"%u",a->delay2);editor(action,"Escalation delay in minutes",value,false,"0–180 minutes after the initial push. Sends a second push if unacknowledged.");
        break;
    case MC_ALERT_PRIORITY:snprintf(value,sizeof(value),"%u",a->ntfy_prio);editor(action,"Push priority",value,false,"1 lowest, 3 default, 5 highest. Phone settings determine sound and silent-mode behavior.");
        break;
    case MC_SMB_HOST:editor(action,"SMB host",p->smb_host,false,"Server hostname or IPv4 address, without a share path.");
        break;
    case MC_SMB_SHARE:editor(action,"SMB share",p->smb_share,false,"Share name only, without slashes.");
        break;
    case MC_SMB_PATH:editor(action,"SMB remote path",p->smb_path,false,"Path inside the share. The preview updates as you type.");
        break;
    case MC_SMB_USER:editor(action,"SMB username",p->smb_user,false,"Blank username uses guest access.");
        break;
    case MC_SMB_PASSWORD:editor(action,"SMB password","",true,"Enter a new password; blank preserves the saved password.");
        break;
    case MC_SHQ_ID:editor(action,"SleepHQ client ID",p->shq_client_id,false,"Your SleepHQ API client ID.");
        break;
    case MC_SHQ_SECRET:editor(action,"SleepHQ client secret","",true,"Enter a new secret; blank preserves the saved value.");
        break;
    case MC_FTP_USER:editor(action,"FTP username",p->ftp_user,false,"Saved settings apply after restart.");
        break;
    case MC_FTP_PASSWORD:editor(action,"FTP password","",true,"Saved settings apply after restart. FTP transmits credentials without encryption.");
        break;
    case MC_UPLOAD_DAYS:snprintf(value,sizeof(value),"%d",p->max_days);editor(action,"Upload history window",value,false,"Days to scan, newest first. Existing recordings and receipts are retained.");
        break;
    default:
        if(action>=A_ZONE0&&action<=A_ZONE5){int i=action-A_ZONE0;if(i<(int)u->live.zone_count){request(MC_TIMEZONE_SET,u->live.zones[i].id,0);u->view=V_TIME;}}
        else if(action>=0&&action<=MC_UPLOAD_RETRY)request(action,"",0);
        break;
    }
    u->dirty=true;
}
static bool subtree_pressed(lv_obj_t *object)
{
    if (lv_obj_has_state(object, LV_STATE_PRESSED)) return true;
    uint32_t children = lv_obj_get_child_cnt(object);
    for (uint32_t i = 0; i < children; ++i)
        if (subtree_pressed(lv_obj_get_child(object, i))) return true;
    return false;
}
static void tick(lv_timer_t *timer)
{
    (void)timer;
    if(!s_ui)return;
    if(touch_manage_config_snapshot(&s_ui->live)) {
        bool rebuild=(s_ui->was_busy&&!s_ui->live.busy)||
            (s_ui->view==V_TEST&&(s_ui->drawn_test_state!=s_ui->live.test.state || s_ui->drawn_test_mask!=(s_ui->live.test.completed_mask | (s_ui->live.test.failed_mask<<8))))||
            (s_ui->view==V_SCAN&&s_ui->drawn_scan_state!=s_ui->live.scan.state);
        if (s_ui->was_busy && !s_ui->live.busy) s_ui->result_until = lv_tick_get() + 8000;
        s_ui->was_busy=s_ui->live.busy;
        if(rebuild&&s_ui->view!=V_EDITOR&&s_ui->view!=V_CONFIRM)s_ui->dirty=true;
    }
    /* A saved-network row resolves tap versus drag on RELEASED. Retiring it
     * while the controller completes a refresh loses that release. Keep the
     * pending render and all native targets until their press has ended. */
    if(s_ui->dirty && !subtree_pressed(s_ui->root))render();
    refresh_dynamic();
}
esp_err_t touch_manage_config_show(lv_obj_t *parent,manage_config_page_t page)
{
    esp_err_t result=touch_manage_config_start();if(result!=ESP_OK)return result;
    touch_manage_config_hide();
    s_ui=heap_caps_calloc(1,sizeof(*s_ui),MALLOC_CAP_SPIRAM);if(!s_ui)return ESP_ERR_NO_MEM;
    s_ui->root=lv_obj_create(parent);lv_obj_set_pos(s_ui->root,0,0);lv_obj_set_size(s_ui->root,768,450);
    lv_obj_set_style_bg_color(s_ui->root,lv_color_hex(BASE),0);lv_obj_set_style_border_width(s_ui->root,0,0);lv_obj_set_style_pad_all(s_ui->root,0,0);lv_obj_set_style_radius(s_ui->root,28,0);lv_obj_set_style_clip_corner(s_ui->root,true,0);lv_obj_clear_flag(s_ui->root,LV_OBJ_FLAG_SCROLLABLE);
    s_ui->view=page==MC_ALERTS?V_ALERTS:page==MC_UPLOADS?V_UPLOADS:page==MC_TIME?V_TIME:V_WIFI;
    touch_manage_config_snapshot(&s_ui->live);s_ui->dirty=true;render();
    s_ui->timer=lv_timer_create(tick,200,NULL);request(MC_REFRESH,"",0);return ESP_OK;
}
void touch_manage_config_hide(void)
{
    if(!s_ui)return;
    if(s_ui->timer)lv_timer_del(s_ui->timer);
    if(s_ui->root)lv_obj_del(s_ui->root);
    memset(s_ui,0,sizeof(*s_ui));free(s_ui);s_ui=NULL;
}
void touch_manage_config_refresh(void) { if(s_ui)tick(NULL); }
