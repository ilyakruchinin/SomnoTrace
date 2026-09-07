/* SomnoTrace O2 Ring storage internals. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Hold this across a complete O2 filesystem transaction.  The export lease
 * deliberately permits concurrent therapy recording, while excluding format,
 * unmount/reboot and other replace-in-place O2 operations. */
bool ox_store_begin_io(uint32_t timeout_ms);
void ox_store_end_io(void);

void ox_store_ensure_dirs(void);
bool ox_store_load_paired(char *serial, size_t serial_sz,
                          char *firmware, size_t fw_sz,
                          char *name_prefix, size_t prefix_sz,
                          char *last_addr, size_t addr_sz,
                          char *driver, size_t driver_sz,
                          char *ble_name, size_t ble_name_sz);
void ox_store_save_paired(const char *serial, const char *firmware,
                          const char *name_prefix, const char *last_addr,
                          const char *driver, const char *ble_name);
void ox_store_delete_paired(void);
int ox_store_index_check(const char *serial, const char *name);
int ox_store_index_conversion_check(const char *serial, const char *name);
void ox_store_index_add(const char *serial, const char *name,
                        uint32_t bytes, bool finalised);
void ox_store_index_mark_converted(const char *serial, const char *name,
                                   bool converted, const char *error);
char *ox_store_conversion_diagnostics_json(void);
long ox_store_part_size(const char *name);
esp_err_t ox_store_part_append(const char *name, const uint8_t *data, size_t len);
bool ox_store_promote(const char *serial, const char *name);
bool ox_store_promote_vld3(const char *serial, const char *name);
bool ox_store_finalize_native(const char *serial, const char *name,
                              long declared_size);
void ox_store_part_remove(const char *name);
