# Arduino-cli → ESP-IDF Port Playbook

The exact recipe used to bring this Box-3 project from arduino-cli to
ESP-IDF v5.5, captured so the same playbook can drive the other TI
host projects (`ti-basic-otg`, `ti-extended-basic-esp32`) and any
future board variants. Every step here corresponds to a real
diagnostic or commit on this branch.

## Why bother

The arduino-cli build hit a DRAM-fragmentation wall: WiFi static
buffers + NimBLE + AsyncTCP + the audio task + a sprite engine left
~80 KB free heap at boot, and any short burst of allocations would
fragment it past usability. arduino-cli's FQBN can't reach the
sdkconfig knobs that control those static pools, so the only path to
headroom was to take ownership of the build system.

After the port, this Box-3 image boots with **107 KB free DRAM** —
WiFi pool cut from 10/32 to 6/6, NimBLE capped at 2 peers, lwIP
sockets at 8, AsyncTCP queue at 16. Same .ino code, same .ino-style
setup()/loop() flow.

## Prereqs

* **ESP-IDF v5.5.4** at `C:\Espressif\v5.5.4\esp-idf` (the user's
  install). PowerShell environment loaded via
  `. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1`.
* **arduino-esp32 v3.3+** as an ESP-IDF component (pulled by the
  Component Manager automatically — `main/idf_component.yml` declares
  the dependency).
* Local **arduino-esp32 v2.0.18** install via arduino-cli stays
  authoritative for the reference build, but is NOT used by the IDF
  project.

## Project scaffold

```
my-project-idf/
├── CMakeLists.txt              # top-level, sets EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults          # board-specific tuning, seeds sdkconfig
├── partitions.csv              # carry over from arduino-cli build
├── main/
│   ├── CMakeLists.txt          # registers main + PRIV_REQUIRES
│   ├── idf_component.yml       # depends on espressif/arduino-esp32
│   ├── main.cpp                # renamed .ino, with edits below
│   └── (audio.cpp, web_files.cpp, ble_keyboard.h, etc — host-specific)
└── components/
    ├── arduino/                # local shim, see below
    ├── ti-emulator/            # submodule: TI_EB_Emulator_ESP
    ├── LovyanGFX/              # submodule: JonVogel/LovyanGFX (fork)
    ├── NimBLE-Arduino/         # submodule: JonVogel/NimBLE-Arduino (fork)
    ├── AsyncTCP/               # submodule: JonVogel/AsyncTCP (fork)
    └── ESPAsyncWebServer/      # submodule: JonVogel/ESPAsyncWebServer (fork)
```

### Top-level `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    components                              # local components
    components/ti-emulator/components       # the 5 TI layers exposed by
                                            # TI_EB_Emulator_ESP's inner
                                            # components/ dir
)

# Match arduino-cli FQBN's CDCOnBoot=cdc. Without these, Arduino's
# Serial routes to UART0 (which has no bridge on the Box-3) and is
# silent. With both at 1, Serial is HWCDCSerial — same USB-Serial-JTAG
# endpoint as the IDF console.
add_compile_definitions(
    ARDUINO_USB_CDC_ON_BOOT=1
    ARDUINO_USB_MODE=1
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my-project)
```

### `main/idf_component.yml`

```yaml
dependencies:
  espressif/arduino-esp32:
    version: "^3.3.1"
```

### `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.cpp" "audio.cpp" "web_files.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        arduino                # the local components/arduino/ shim
        interpreter            # TI Extended BASIC engine
        font                   # TI 8x8 font
        speech                 # (omit on boards without audio codec)
        ble_hid_host           # BLE HID host
        file_handling          # LittleFS / SD / V9T9 DSK
        LovyanGFX
        NimBLE-Arduino
        ESPAsyncWebServer      # (omit on boards without WiFi UI)
        AsyncTCP               # (omit on boards without WiFi UI)
)
```

### `components/arduino/CMakeLists.txt` (the shim)

```cmake
# Wraps the IDF Component Manager copy of arduino-esp32 under the
# short name "arduino". Without this, downstream components would
# need to REQUIRE espressif__arduino-esp32 (the namespaced name) which
# is awkward to write in every submodule's CMakeLists.
idf_component_register(REQUIRES espressif__arduino-esp32)
```

## `sdkconfig.defaults` — the knobs that matter

Most of these are non-default settings discovered the hard way.

```ini
# --- Target + flash ---
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y   # adjust to 8MB / 4MB per board
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y

CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# --- PSRAM (Box-3: 16MB octal; OTG: none — omit this whole block) ---
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=n           # keep PSRAM opt-in
CONFIG_SPIRAM_USE_CAPS_ALLOC=y

# --- USB console (boards with no UART bridge use USB-Serial-JTAG) ---
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# --- WiFi static pool (cut from 10/32 defaults) ---
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=8
CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n
CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n

# --- lwIP ---
CONFIG_LWIP_MAX_SOCKETS=8
CONFIG_LWIP_TCP_WND_DEFAULT=4096     # note: renamed from RCV_BUF_DEFAULT in IDF v5.5
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=4096
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=2560

# --- FATFS Long File Names — without this, SAVE to SDCARD fails for any name > 8.3 ---
CONFIG_FATFS_LFN_HEAP=y

# --- NimBLE: cap connections + mute INFO chatter ---
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2
CONFIG_BT_NIMBLE_MAX_BONDS=4
CONFIG_BT_NIMBLE_MAX_CCCDS=8
# CONFIG_BT_NIMBLE_LOG_LEVEL_INFO is not set
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y

# --- CPU + scheduling ---
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000

# --- Task watchdog (both IDLE checks ON, app must yield) ---
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y

# --- Arduino-as-component ---
CONFIG_AUTOSTART_ARDUINO=y           # spins setup()/loop() task on its own — no manual app_main needed
CONFIG_ARDUINO_RUNNING_CORE=1
CONFIG_ARDUINO_EVENT_RUNNING_CORE=1
CONFIG_ARDUINO_LOOP_STACK_SIZE=16384
```

**`sdkconfig.defaults` only seeds the initial sdkconfig.** If you
already have a generated `sdkconfig`, edits to `sdkconfig.defaults`
don't reapply. To re-seed, `rm sdkconfig` then `idf.py build`. Or use
`idf.py menuconfig` to edit the live one.

## .ino → main.cpp conversion

Rename `your-sketch.ino` to `main/main.cpp`. Then apply these edits
(every one corresponds to a real bug we hit).

### 1. Header includes — drop subdir prefixes

Arduino's library system flattens; ESP-IDF treats each component's
`INCLUDE_DIRS "."` as the search root. So:

```cpp
// Arduino style:
#include "TI_Extended_Basic_Interpreter/ti_platform.h"
#include "ESP32_File_Handling/file_io.h"

// IDF style — both expose the headers at their component root:
#include "ti_platform.h"
#include "file_io.h"
```

If the header might be at either path (e.g. for sibling builds), use
`__has_include`:
```cpp
#if __has_include("TI_Speech/speech_rom.h")
  #include "TI_Speech/speech_rom.h"
#elif __has_include("speech_rom.h")
  #include "speech_rom.h"
#endif
```

### 2. Forward declarations

The Arduino IDE auto-inserts forward declarations for everything in the
.ino. Plain C++ doesn't. Add a block near the top:

```cpp
struct LineEdit;
static void fillBackground(uint16_t bg);
static void drawPairingScreen();
// ... etc — one per function used before its definition
```

### 3. Routing `Serial` to USB-Serial-JTAG

Already handled by the `ARDUINO_USB_CDC_ON_BOOT=1` + `ARDUINO_USB_MODE=1`
defines at the top-level CMakeLists. Without them, `Serial.print()`
goes to UART0 — silent on boards without a UART bridge.

### 4. The cooperative-yield contract

ESP-IDF v5 with both IDLE-task watchdogs enabled will fire after 5 s
if your `loop()` (or a long-running BASIC command) doesn't yield.
Arduino's `yield()` and FreeRTOS's `taskYIELD()` only switch among
same-or-higher-priority tasks — they **don't** let IDLE (lowest
priority) run. You need `delay(1)` (= `vTaskDelay(1 tick)`) to put the
calling task into the Blocked state for one tick so IDLE can run.

The TI interpreter calls a host-supplied weak hook `tiYield()` after
every BASIC line. Define a strong override in main.cpp:

```cpp
void tiYield() { delay(1); }
```

Then call `tiYield()` at the end of every host loop that can run for
hundreds of ms or more — the main `loop()`, file-listing loops,
program save/load iterators, screen redraw, etc. See
`web_files.cpp::tick()` and `catPrintLine()` in this repo for
examples.

### 5. Logging — silence the noisy subsystems

Both NimBLE and WiFi spam INFO lines during normal operation. Bury
them with a runtime call at the top of `setup()`:

```cpp
#include <esp_log.h>
// ...
esp_log_level_set("NimBLE",         ESP_LOG_WARN);
esp_log_level_set("BLE_INIT",       ESP_LOG_WARN);
esp_log_level_set("wifi",           ESP_LOG_WARN);
esp_log_level_set("wifi_init",      ESP_LOG_WARN);
esp_log_level_set("wpa",            ESP_LOG_WARN);
esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
esp_log_level_set("dhcpc",          ESP_LOG_WARN);
esp_log_level_set("phy_init",       ESP_LOG_WARN);
```

## Submodules to add

```bash
# In your IDF project root:
git submodule add -b main     https://github.com/JonVogel/TI_EB_Emulator_ESP.git  components/ti-emulator
git submodule add -b idf-port https://github.com/JonVogel/LovyanGFX.git           components/LovyanGFX
git submodule add -b idf-port https://github.com/JonVogel/NimBLE-Arduino.git      components/NimBLE-Arduino
git submodule add -b idf-port https://github.com/JonVogel/AsyncTCP.git            components/AsyncTCP
git submodule add -b idf-port https://github.com/JonVogel/ESPAsyncWebServer.git   components/ESPAsyncWebServer
git submodule update --init
```

## Build + flash

```powershell
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
cd c:\dev\repos\my-project-idf

# On Windows, set this BEFORE build — IDF's parallel ninja can trigger
# a GCC ICE in esp-dsp's dspi_conv_f32_ansi.c under high parallelism.
$env:CMAKE_BUILD_PARALLEL_LEVEL=2

idf.py build
idf.py -p COM19 flash monitor    # adjust COM port per board
```

## Gotchas — every one of these cost real debug time

| Symptom | Cause | Fix |
|---|---|---|
| `LovyanGFX init returned ok=1` but `tft.fillScreen()` crashes LoadProhibited | LovyanGFX's `class LGFXBase : public Print` is conditional on `ARDUINO` macro; main.cpp sees it (REQUIRES arduino), but LovyanGFX itself compiles without `ARDUINO` defined → different class layouts → setPanel writes to a different offset than getPanel reads | LovyanGFX component must `REQUIRES arduino` to inherit the `-DARDUINO=10812` PUBLIC compile def. Our fork's `boards.cmake/esp-idf.cmake` already does this. |
| `BLE_INIT: controller init failed` then crash in `nimble_port_run` | Both IDF NimBLE host and NimBLE-Arduino bundled stack are linked; both call `esp_bt_controller_init` | Our `JonVogel/NimBLE-Arduino` fork bundles the full NimBLE stack and is set up so the linker prefers the bundled `nimble_port.c` (which has `#if false // Arduino disable` around the controller init). Keep `CONFIG_BT_NIMBLE_ENABLED=y` so IDF provides the transport layer (ble_transport_ll_init). |
| `[E][vfs_api.cpp] fopen(/sdcard/SOMELONGNAME.bas) failed` | `CONFIG_FATFS_LFN_NONE=y` (IDF default) — only 8.3 names | `CONFIG_FATFS_LFN_HEAP=y` |
| `ESP_ERR_INVALID_STATE` when `Wire.begin()` after LovyanGFX init | Touch_GT911 in LovyanGFX claims `i2c_master_bus_handle_t` on I2C_NUM_0 (new IDF I2C driver); Wire then tries to install its own on the same port | Disable touch in LovyanGFX's Box-3 detector. Our fork already does this. Real fix is bus-handle sharing, which is more work. |
| Task watchdog fires for `IDLE1 (CPU 1)` every 5 s | `loop()` runs flat-out, never yields to lower-priority tasks | `delay(1)` at the end of `loop()` AND in every long-running BASIC command. See `tiYield()` weak hook in interpreter. |
| GCC ICE on `dspi_conv_f32_ansi.c:184` during build | Memory pressure during parallel build on Windows | `$env:CMAKE_BUILD_PARALLEL_LEVEL=2` before `idf.py build` |
| `Serial.println(...)` silent | `ARDUINO_USB_CDC_ON_BOOT=0` (default) — Serial routes to UART0 | `add_compile_definitions(ARDUINO_USB_CDC_ON_BOOT=1 ARDUINO_USB_MODE=1)` at top-level CMakeLists |
| `arduino` component not found at configure time | Tried `REQUIRES arduino` but didn't add the shim | Create `components/arduino/CMakeLists.txt` with `idf_component_register(REQUIRES espressif__arduino-esp32)` |
| `Required to lock TCPIP core functionality` panic from AsyncTCP | Vanilla `AsyncTCP` doesn't lock TCPIP core in `tcp_alloc` → assertion fails | Use `mathieucarbou/AsyncTCP` (our fork's idf-port branch). |

## Board-specific differences

### Box-3 vs OTG (sample of where they diverge)

| | Box-3 | OTG |
|---|---|---|
| Flash | 16 MB | 8 MB |
| PSRAM | 16 MB octal | none — drop `CONFIG_SPIRAM*` |
| LCD | 320×240 ILI9342C | 240×240 ST7789 |
| SD | SD_MMC 1-bit | SD over SPI |
| Audio codec | ES8311 | none — drop `components/speech` etc. |
| Buttons | BOOT + MUTE | OK + UP + DOWN + MENU |
| LEDs | none usable | green + yellow |
| WiFi file mgr | yes | no |

LovyanGFX has autodetect profiles for both boards (Box-3 V3 was already
there; we added OTG in `JonVogel/LovyanGFX@idf-port`). Same submodule,
both projects.

## See also

* `JonVogel/esp32-s3-box-basic-idf` — the reference Box-3 IDF port (this repo)
* `JonVogel/TI_EB_Emulator_ESP` — the 5 shared layers
* `JonVogel/LovyanGFX@idf-port` — LovyanGFX fork with patches
* `JonVogel/NimBLE-Arduino@idf-port` — NimBLE-Arduino fork with bundled stack
* `JonVogel/AsyncTCP@idf-port`, `JonVogel/ESPAsyncWebServer@idf-port` — Web file manager
