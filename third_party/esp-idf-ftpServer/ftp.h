/*
 * This file is part of the MicroPython ESP32 project, https://github.com/loboris/MicroPython_ESP32_psRAM_LoBo
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 LoBo (https://github.com/loboris)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * This file is based on 'ftp' from Pycom Limited.
 *
 * Author: LoBo, loboris@gmail.com
 * Copyright (c) 2017, LoBo
 */

#ifndef FTP_H_
#define FTP_H_

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <dirent.h>

/******************************************************************************
 DEFINE PRIVATE CONSTANTS
 ******************************************************************************/
#define FTP_CMD_PORT                        21
#define FTP_ACTIVE_DATA_PORT                20
#define FTP_PASIVE_DATA_PORT                2024
#define FTP_CMD_SIZE_MAX                    6
#define FTP_CMD_CLIENTS_MAX                 1
#define FTP_DATA_CLIENTS_MAX                1
#define FTP_MAX_PARAM_SIZE                  (MICROPY_ALLOC_PATH_MAX + 1)
#define FTP_UNIX_SECONDS_180_DAYS           15552000
#define FTP_DATA_TIMEOUT_MS                 10000   // 10 seconds
#define FTP_SOCKETFIFO_ELEMENTS_MAX         4


typedef enum {
    E_FTP_STE_DISABLED = 0,
    E_FTP_STE_START,
    E_FTP_STE_READY,
    E_FTP_STE_END_TRANSFER,
    E_FTP_STE_CONTINUE_LISTING,
    E_FTP_STE_CONTINUE_FILE_TX,
    E_FTP_STE_CONTINUE_FILE_RX,
    E_FTP_STE_CONNECTED
} ftp_state_t;

typedef enum {
    E_FTP_STE_SUB_DISCONNECTED = 0,
    E_FTP_STE_SUB_LISTEN_FOR_DATA,
    E_FTP_STE_SUB_DATA_CONNECTED
} ftp_substate_t;


/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/
typedef enum {
    E_FTP_RESULT_OK = 0,
    E_FTP_RESULT_CONTINUE,
    E_FTP_RESULT_FAILED
} ftp_result_t;

typedef struct {
    bool            uservalid : 1;
    bool            passvalid : 1;
} ftp_loggin_t;

typedef enum {
    E_FTP_NOTHING_OPEN = 0,
    E_FTP_FILE_OPEN,
    E_FTP_DIR_OPEN
} ftp_e_open_t;

typedef enum {
    E_FTP_CLOSE_NONE = 0,
    E_FTP_CLOSE_DATA,
    E_FTP_CLOSE_CMD_AND_DATA,
} ftp_e_closesocket_t;

typedef struct {
    uint8_t         *dBuffer;
    uint32_t        ctimeout;
    union {
        DIR         *dp;
        FILE        *fp;
    };
    int32_t         lc_sd;
    int32_t         ld_sd;
    int32_t         c_sd;
    int32_t         d_sd;
    int32_t         dtimeout;
    uint32_t        ip_addr;
    uint8_t         state;
    uint8_t         substate;
    uint8_t         txRetries;
    uint8_t         logginRetries;
    ftp_loggin_t    loggin;
    uint8_t         e_open;
    bool            closechild;
    bool            enabled;
    bool            listroot;
    uint32_t        total;
    uint32_t        time;
} ftp_data_t;

typedef struct {
    char * cmd;
} ftp_cmd_t;

typedef enum {
    E_FTP_CMD_NOT_SUPPORTED = -1,
    E_FTP_CMD_FEAT = 0,
    E_FTP_CMD_SYST,
    E_FTP_CMD_CDUP,
    E_FTP_CMD_CWD,
    E_FTP_CMD_PWD,
    E_FTP_CMD_XPWD,
    E_FTP_CMD_SIZE,
    E_FTP_CMD_MDTM,
    E_FTP_CMD_TYPE,
    E_FTP_CMD_USER,
    E_FTP_CMD_PASS,
    E_FTP_CMD_PASV,
    E_FTP_CMD_LIST,
    E_FTP_CMD_RETR,
    E_FTP_CMD_STOR,
    E_FTP_CMD_DELE,
    E_FTP_CMD_RMD,
    E_FTP_CMD_MKD,
    E_FTP_CMD_RNFR,
    E_FTP_CMD_RNTO,
    E_FTP_CMD_NOOP,
    E_FTP_CMD_QUIT,
    E_FTP_CMD_APPE,
    E_FTP_CMD_NLST,
    E_FTP_CMD_AUTH,
    E_FTP_NUM_FTP_CMDS
} ftp_cmd_index_t;



#define FTP_USER_PASS_LEN_MAX	32
#define FTP_MUTEX_TIMEOUT_MS    1000
#define FTP_CMD_TIMEOUT_MS      (300*1000)

#define CONFIG_MICROPY_FTPSERVER_BUFFER_SIZE 4096
#define MICROPY_ALLOC_PATH_MAX (512)

// #define MAX_ACTIVE_INTERFACES   3
//tcpip_adapter_if_t tcpip_if[MAX_ACTIVE_INTERFACES] = {TCPIP_ADAPTER_IF_MAX};

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#if 0
extern const char *FTP_TAG;
extern uint32_t ftp_stack_size;
extern QueueHandle_t ftp_mutex;
extern int ftp_buff_size;
extern int ftp_timeout;
#endif

/* Credentials — set these before calling ftp_server_start() to override
 * the default anonymous login. */
extern char ftp_user[FTP_USER_PASS_LEN_MAX + 1];
extern char ftp_pass[FTP_USER_PASS_LEN_MAX + 1];
extern bool ftp_anonymous_mode;  /* true: accept anonymous/ftp + any password;
                                  * false: require exact user/pass match */

bool ftp_init (void);
void ftp_deinit (void);
int ftp_run (uint32_t elapsed);
bool ftp_enable (void);
bool ftp_isenabled (void);
bool ftp_disable (void);
bool ftp_reset (void);
int ftp_getstate();
bool ftp_terminate (void);
bool ftp_stop_requested();

/* Start the FTP server task. Call after Wi-Fi is connected and SD card is mounted. */
void ftp_task(void *pvParameters);

static inline void ftp_server_start(void) {
    StackType_t *stack = heap_caps_malloc(1024*6, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb  = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (stack && tcb) {
        xTaskCreateStaticPinnedToCore(ftp_task, "ftp_srv", 1024*6, NULL, 2, stack, tcb, tskNO_AFFINITY);
    } else {
        free(stack); free(tcb);
    }
}

#endif /* FTP_H_ */
