# libsmb2

- **Source:** https://github.com/sahlberg/libsmb2
- **Version / Tag:** libsmb2-6.2
- **Upstream Author:** Ronnie Sahlberg and contributors
- **License:** LGPL-2.1 (core library) / BSD-2-Clause (examples/tools)
- **Used for:** SMB2/SMB3 client library for uploading generated EDF files to network shares

## Modifications

The following changes were made to integrate libsmb2 into the ESP-IDF v5.5 build system:
- Added ESP-IDF compatible `CMakeLists.txt` and `idf_component.yml`
- Added `include/esp/config.h` with ESP32-specific configuration (`_U_`, `SOL_TCP`, `HAVE_SYS_TIME_H`)
- Excluded POSIX command-line utilities and tests from the firmware build
