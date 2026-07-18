# display_lgfx_box — Box-3 SPI panel driver + TiDisplay hook wrappers.
#
# One of a family of `display_XXX` components: each one owns the tft
# object for its panel, provides the init() function main.cpp calls,
# and fills in the TiDisplay hooks host_common dispatches through.
# Compile-time selection is just "which display_XXX does main/CMakeLists
# put in PRIV_REQUIRES?".
#
# For box the panel is LovyanGFX's AUTODETECT class — LGFX picks the
# right panel driver for the Box-3 SPI screen (ILI9342C via
# LGFX_ESP32S3_BOX_v3 in LovyanGFX 1.2.x) at runtime.
idf_component_register(
    SRCS
        "src/display_box.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public API: hosts include display_box.h + link against the
        # tft global. Both are declared via ti_host.h types.
        host_common
        # LovyanGFX for the panel driver. Kept as a REQUIRES (not
        # PRIV_REQUIRES) so main.cpp's remaining direct `tft.` calls
        # for the boot screen / pairing UI still resolve through this
        # component's public include of LovyanGFX.hpp. When those
        # move into host_common in a later commit, this can drop to
        # PRIV_REQUIRES.
        LovyanGFX
        # arduino for delay() + pinMode() during init.
        arduino
)
