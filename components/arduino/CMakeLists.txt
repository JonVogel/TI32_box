# Shim component so the rest of the build can `REQUIRES arduino` while
# the actual sources come from the IDF Component Manager copy of
# arduino-esp32 (managed_components/espressif__arduino-esp32). The
# component manager registers it under its namespaced directory name
# `espressif__arduino-esp32`, which is awkward to reference from every
# sibling-submodule CMakeLists. This thin alias keeps those clean.
idf_component_register(
    REQUIRES espressif__arduino-esp32
)
