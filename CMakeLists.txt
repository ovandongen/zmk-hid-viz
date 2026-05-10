if(CONFIG_HID_VIZ)
    zephyr_library()

    # Transport layer (always included)
    zephyr_library_sources(src/events.c)
    zephyr_library_sources(src/usb_hid.c)

    if(CONFIG_ZMK_BLE)
        zephyr_library_sources(src/hog.c)
    endif()

    # Notifier (layer + key events)
    if(CONFIG_HID_VIZ_LAYER_EVENTS OR CONFIG_HID_VIZ_KEY_EVENTS)
        zephyr_library_sources(src/notifier.c)
    endif()

    # Command handler (inbound from host)
    if(CONFIG_HID_VIZ_COMMANDS)
        zephyr_library_sources(src/command_handler.c)
    endif()

    zephyr_include_directories(include)
    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
