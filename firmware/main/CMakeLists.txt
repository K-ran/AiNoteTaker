idf_component_register(
    SRCS
        "main.c"
        "core/device_config.c"
        "core/event_log.c"
        "core/timekeep.c"
        "core/console.c"
        "board/rtc_pcf85063.c"
        "board/battery.c"
        "hardeware_driver/bsp_board.c"
        "tca9555_driver/tca9555_driver.c"
        "audio/capture.c"
        "audio/ogg_opus.c"
        "audio/recorder.c"
        "storage/chunk_store.c"
        "net/wifi_link.c"
        "net/smaarthi_api.c"
        "net/uplink.c"
        "net/setup_portal.c"
        "ui/status_led.c"
        "ui/buttons.c"
    INCLUDE_DIRS
        "." "core" "board" "hardeware_driver" "tca9555_driver" "audio" "storage" "net" "ui"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Vendor BSP files are kept as shipped; silence their warnings so ours stay visible.
set_source_files_properties("hardeware_driver/bsp_board.c" "tca9555_driver/tca9555_driver.c" PROPERTIES COMPILE_OPTIONS "-w")
