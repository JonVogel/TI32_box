# ESP-IDF component glue. Routes BASIC file I/O to LittleFS / SD /
# mounted V9T9 DSK images. Headers + program_io.cpp depend on Arduino's
# fs::FS abstraction (LittleFS / SD_MMC / SD via SPI).
idf_component_register(
    SRCS "program_io.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino
)
