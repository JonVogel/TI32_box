set(COMPONENT_SRCDIRS
    "src"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "src"
)

set(COMPONENT_REQUIRES
    "arduino"
)

register_component()

target_compile_options(${COMPONENT_TARGET} PRIVATE -fno-rtti)

# Cap the async event queue at 16 (library default is 64). Matches the
# arduino-cli build.bat extra-flags override on the Arduino side.
target_compile_definitions(${COMPONENT_TARGET} PUBLIC CONFIG_ASYNC_TCP_QUEUE_SIZE=16)
