# In order for the cases defined by `TEST_CASE` to be linked into the final elf,
# the component can be registered as WHOLE_ARCHIVE
idf_component_register(
    SRCS "test_app_main.c" "test_uart.c" "test_uart_auto_lightsleep.c"
    REQUIRES driver unity esp_pm
    PRIV_INCLUDE_DIRS .
    WHOLE_ARCHIVE
)
