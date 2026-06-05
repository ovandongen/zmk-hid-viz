/*
 * Linker fragment: defines the ROM iterable section that collects every
 * HID_VIZ_CAP_REGISTER() entry (struct hid_viz_cap) from the compiled-in
 * source files. manifest.c walks it with STRUCT_SECTION_FOREACH(hid_viz_cap, ...).
 *
 * Injected via `zephyr_linker_sources(SECTIONS src/hid_viz_cap.ld)` in
 * CMakeLists.txt. ITERABLE_SECTION_ROM emits the __hid_viz_cap_list_start/_end
 * boundary symbols and KEEPs the (sorted-by-name) entries even with link-time
 * garbage collection. Harmless when empty (start == end).
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(hid_viz_cap, Z_LINK_ITERABLE_SUBALIGN)
