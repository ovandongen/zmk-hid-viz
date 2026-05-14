if(CONFIG_HID_VIZ)
    zephyr_library()

    if(CONFIG_HID_VIZ_LAYER_EVENTS OR CONFIG_HID_VIZ_KEY_EVENTS)
        zephyr_library_sources(src/notifier.c)
    endif()

    if(CONFIG_HID_VIZ_COMMANDS)
        zephyr_library_sources(src/command_handler.c)
    endif()

    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
