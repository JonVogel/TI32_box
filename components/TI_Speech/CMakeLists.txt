# ESP-IDF component glue. The TMS5220 LPC-10 synth (ported from MAME)
# plus the speech_rom.h baked from a TI Speech Synthesizer cartridge
# dump. Depends only on Arduino (for pgm_read_byte / PROGMEM and the
# host's int16_t / int32_t typedefs).
idf_component_register(
    SRCS "tms5220.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino
)
