# ESP-IDF component glue. The interpreter is header-only except for
# ti_platform.cpp which defines weak no-op platform hooks (strong
# overrides live in the host project). Headers depend on Arduino for
# delay()/millis()/Stream/etc.
idf_component_register(
    SRCS "ti_platform.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino
)

# exec_manager.h has a constructor whose init-list order doesn't match
# the member-declaration order. The behavior is correct (all members
# are POD), but IDF's default flags promote -Wreorder to an error.
# token_parser.h also has snprintf calls the compiler can't prove
# fit in the destination buffers — same story, arduino-cli's default
# flags don't promote -Wformat-truncation to an error. Suppress both
# at INTERFACE level so dependents that include our headers inherit it.
target_compile_options(${COMPONENT_LIB} INTERFACE
    -Wno-error=reorder
    -Wno-error=format-truncation
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=reorder
    -Wno-error=format-truncation
)
