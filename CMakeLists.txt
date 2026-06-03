if(CONFIG_HID_VIZ)
    if(NOT CONFIG_RAW_HID)
        message(FATAL_ERROR "zmk-hid-viz requires CONFIG_RAW_HID=y. Add zmk-raw-hid to your west projects list and enable CONFIG_RAW_HID=y in your .conf.")
    endif()

    if(CONFIG_HID_VIZ_LAYER_EVENTS OR CONFIG_HID_VIZ_KEY_EVENTS)
        target_sources(app PRIVATE src/notifier.c)
    endif()

    if(CONFIG_HID_VIZ_COMMANDS)
        target_sources(app PRIVATE src/command_handler.c)
        target_sources(app PRIVATE src/layer_actions.c)
    endif()

    if(CONFIG_HID_VIZ_MANIFEST)
        target_sources(app PRIVATE src/manifest.c)
    endif()
endif()
