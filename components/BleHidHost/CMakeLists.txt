# ESP-IDF component glue. Header-only, depends on NimBLE-Arduino (BLE
# host stack) and Arduino (Preferences NVS for bond persistence).
idf_component_register(
    INCLUDE_DIRS "."
    REQUIRES arduino NimBLE-Arduino
)
