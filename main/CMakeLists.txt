# Main component. The .ino is split into a small app_main() trampoline
# in main.cpp that initializes the Arduino layer and then calls
# setup()/loop() — that lets us keep the Arduino lifecycle for the
# rest of the host code without rewriting against raw IDF APIs.
idf_component_register(
    SRCS
        "main.cpp"
        "audio.cpp"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        # Arduino layer (added via idf_component.yml) gives us
        # LittleFS, SD_MMC, Preferences, WiFi, Wire, etc. and pulls in
        # the ESP-IDF components those need transitively. The component
        # manager fetches it as managed_components/espressif__arduino-esp32;
        # we wrap that with the local components/arduino/ shim so child
        # components can keep using the unqualified `arduino` name.
        arduino
        # The 5 TI emulator host-shared layers come from the consolidated
        # TI_EB_Emulator_ESP submodule (components/ti-emulator/). The
        # submodule's own components/ subdir is added to
        # EXTRA_COMPONENT_DIRS at the top-level CMakeLists, so each one
        # is just referenced by name here.
        interpreter            # TI Extended BASIC engine
        font                   # 8x8 TI ROM font + TI logo
        speech                 # TMS5220 LPC-10 synth + cartridge ROM
        ble_hid_host           # NimBLE multi-peer BLE HID host
        file_handling          # LittleFS / SD / V9T9 DSK file I/O
        host_common            # shared host layer (palette, resolveColor,
                               # eventually line editor + tiPrint family +
                               # boot screen + sprites + command dispatch)
        # Third-party Arduino libraries (submodules under components/).
        # LovyanGFX comes in transitively via the display_lgfx_box
        # component's REQUIRES; main.cpp uses it through the
        # display_lgfx_box::tft extern so the direct dep isn't
        # needed here anymore.
        display_lgfx_box
        LovyanGFX
        NimBLE-Arduino
        ESPAsyncWebServer
        AsyncTCP
        # Shared HTTP file manager (JonVogel/TI_Web_Files_ESP @ idf-port).
        # Replaces the previous main/web_files.cpp, which was a local
        # SPA-style implementation. The shared library is JSON-only —
        # use ti_files.py from the same repo on a PC for the UI.
        web_files
)
