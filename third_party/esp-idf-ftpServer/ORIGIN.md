# esp-idf-ftpServer

- **Source:** https://github.com/nopnop2002/esp-idf-ftpServer
- **Upstream author:** nopnop2002 (MIT License)
- **Original author:** LoBo / loboris@gmail.com (MIT License, based on Pycom Limited's FTP code)
- **License:** MIT
- **Used for:** lightweight FTP server for Wi-Fi file transfer to/from SD card

## Modifications

The following changes were made to adapt the code for SomnoTrace:

- `MOUNT_POINT` changed from `/root` to `/somnotrace` (SomnoTrace SD card mount point)
- Removed dependency on `xEventTask` / `FTP_TASK_FINISH_BIT` (external event group)
- `ftp_task` made self-contained — calls `ftp_init()`, `ftp_enable()`, and loops `ftp_run()` internally
- Credentials (`ftp_user` / `ftp_pass`) default to anonymous (`anonymous` / `anonymous@`) instead of requiring Kconfig
- Log level set to `ESP_LOG_WARN` by default to reduce verbosity
- Added `ftp_server_start()` convenience API in a wrapper header
