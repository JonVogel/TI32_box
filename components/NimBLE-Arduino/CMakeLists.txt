# ESP-IDF component glue for NimBLE-Arduino.
#
# NimBLE-Arduino ships its OWN complete NimBLE host stack alongside the
# Arduino C++ wrappers. The bundled stack's nimble_port.c has the
# esp_bt_controller_init() call guarded with `#if false // Arduino disable`
# because NimBLEDevice::init does that step itself. IDF's bundled NimBLE
# host (CONFIG_BT_NIMBLE_ENABLED=y) has NO such guard, so if IDF's
# nimble_port.c is what gets linked, you get a double-init failure.
#
# The fix is to compile BOTH:
#   * the Arduino-style C++ wrappers (src/*.cpp), AND
#   * the bundled NimBLE host C sources under src/nimble/.
# IDF's linker prefers user-component symbols, so the bundled
# nimble_port_init (with the no-double-init guard) wins over IDF's. We
# keep CONFIG_BT_NIMBLE_ENABLED=y so IDF still provides the transport
# layer (ble_transport_ll_init etc.) the bundle references.

# Arduino-facing C++ wrappers.
file(GLOB NIMBLE_ARDUINO_SRCS "src/*.cpp")

# Bundled NimBLE host stack + porting layer + ESP HCI glue + tinycrypt.
# Excludes:
#   - nimble/controller/    (ESP32 uses ROM-based controller via `bt`)
#   - nimble/drivers/nrf*/  (Nordic-only)
#   - nimble/host/mesh/     (BLE Mesh — unused)
#   - nimble/transport/{nrf5340,esp_ipc,esp_ipc_legacy}/  (other targets)
file(GLOB_RECURSE NIMBLE_BUNDLE_C_SRCS
    "src/nimble/porting/nimble/src/*.c"
    "src/nimble/porting/npl/freertos/src/*.c"
    "src/nimble/nimble/host/src/*.c"
    "src/nimble/nimble/host/util/src/*.c"
    "src/nimble/nimble/host/services/gap/src/*.c"
    "src/nimble/nimble/host/services/gatt/src/*.c"
    "src/nimble/nimble/host/store/config/src/*.c"
    "src/nimble/nimble/host/store/ram/src/*.c"
    "src/nimble/nimble/transport/src/*.c"
    "src/nimble/nimble/transport/common/hci_h4/src/*.c"
    "src/nimble/esp_port/esp-hci/src/*.c"
    "src/nimble/esp_port/port/src/*.c"
    "src/nimble/ext/tinycrypt/src/*.c"
)

idf_component_register(
    SRCS ${NIMBLE_ARDUINO_SRCS} ${NIMBLE_BUNDLE_C_SRCS}
    INCLUDE_DIRS
        "src"
        "src/nimble/porting/nimble/include"
        "src/nimble/porting/npl/freertos/include"
        "src/nimble/nimble/include"
        "src/nimble/nimble/host/include"
        "src/nimble/nimble/host/util/include"
        "src/nimble/nimble/host/services/gap/include"
        "src/nimble/nimble/host/services/gatt/include"
        "src/nimble/nimble/host/store/config/include"
        "src/nimble/nimble/host/store/ram/include"
        "src/nimble/nimble/transport/include"
        "src/nimble/nimble/transport/common/hci_h4/include"
        "src/nimble/esp_port/esp-hci/include"
        "src/nimble/esp_port/port/include"
        "src/nimble/esp_port/port/transport/include"
        "src/nimble/ext/tinycrypt/include"
    REQUIRES arduino bt nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fno-rtti)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ESP_PLATFORM=1)
