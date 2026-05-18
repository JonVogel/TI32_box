# ESP-IDF component glue. Header-only — exposes ti_font.h to anyone
# that REQUIRES TI_Font. Arduino-style projects can keep ignoring this
# file and pick up the library via library.properties as before.
idf_component_register(
    INCLUDE_DIRS "."
    REQUIRES arduino
)
