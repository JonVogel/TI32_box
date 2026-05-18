# Main component. The .ino is split into a small app_main() trampoline
# in main.cpp that initializes the Arduino layer and then calls
# setup()/loop() — that lets us keep the Arduino lifecycle for the
# rest of the host code without rewriting against raw IDF APIs.
idf_component_register(
    SRCS
        "main.cpp"
        "audio.cpp"
        "web_files.cpp"
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
        # Our shared Arduino libraries dropped into components/.
        TI_Extended_Basic_Interpreter
        TI_Font
        BleHidHost
        ESP32_File_Handling
        TI_Speech
        # Third-party Arduino libraries (also under components/).
        LovyanGFX
        NimBLE-Arduino
        ESPAsyncWebServer
        AsyncTCP
)
