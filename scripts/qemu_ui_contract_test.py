#!/usr/bin/env python3
"""QEMU board, transport and artifact contracts, before native feature routes."""
from pathlib import Path
from qemu_targets import target
ROOT = Path(__file__).resolve().parents[1]
def source(path): return (ROOT / path).read_text()
cmake=source('main/CMakeLists.txt')
for unit in ('main_qemu.c','board_qemu.c','bsp_display_7b.c','main_qemu_154.c','board_qemu_154.c','bsp_display.c','firmware_target.c'):
    assert f'"{unit}"' in cmake, unit
assert '-u somnotrace_firmware_target' in cmake
assert 'config SOMNOTRACE_QEMU_DISPLAY_154' in source('main/Kconfig.projbuild')
assert target() == target('7b')
assert (target('154')['width'],target('154')['height']) == (240,240)
assert (target('7b')['width'],target('7b')['height']) == (1024,600)
assert 'CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y' in source('sdkconfig.qemu-154.defaults')
assert 'CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y' not in source('sdkconfig.qemu.defaults')
display=source('main/bsp_display_7b.c')
for marker in ('board_qemu_touch_read','QEMU UI first frame published','esp_lcd_rgb_qemu_get_frame_buffer','lv_disp_flush_is_last','LV_OPA_COVER'):
    assert marker in display,marker
board=source('main/board_qemu.c')
for marker in ('WAVESHARE_7B_H_RES','WAVESHARE_7B_V_RES','QEMU_RGB_TOUCH_POSITION','QEMU touch release sampled'):
    assert marker in board,marker
for path in ('main/main_qemu.c','main/main_qemu_154.c'):
    demo=source(path)
    for forbidden in ('first_run_setup','touch_logs','touch_history','touch_maintenance','as11_ble_init','net_provision_init','sd_storage_init'):
        assert forbidden not in demo,(path,forbidden)
setup=source('scripts/setup-qemu-macos.sh')
for marker in ('40edccac415693c5130f91c01d84176ae6008566','qemu-touch.patch','qemu-emulator-stability.patch','PATCH_REVISION="4"','--disable-dbus-display'):
    assert marker in setup,marker
for script in ('scripts/build-qemu.sh','scripts/run-qemu-ui.sh','scripts/test-qemu-ui.sh'):
    text=source(script)
    assert 'qemu_targets.py' in text and '--board "${BOARD}"' in text
assert 'SomnoTrace QEMU is already running' in source('scripts/run-qemu-ui.sh')
for path in ('scripts/run-qemu-ui.sh','scripts/qemu_boot_smoke.py'):
    text=source(path)
    assert 'nvram.esp32s3.efuse' in text and 'timer.esp32s3.timg' in text
assert 'ELF file SHA256:' in source('scripts/qemu_boot_smoke.py')
for identity in ('waveshare-7b','waveshare-154','qemu-ui','qemu-154'):
    assert '"'+identity+'"' in source('main/firmware_target.c')
print('Both QEMU profiles, actual renderers, retained identity and launch contracts passed')
