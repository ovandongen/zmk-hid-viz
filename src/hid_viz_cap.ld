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

/*
 * Subalign is a literal 4, not Z_LINK_ITERABLE_SUBALIGN: in this Zephyr 3.5 /
 * MoErgo build CONFIG_LINKER_ITERABLE_SUBALIGN is undefined, so that macro reaches
 * the linker as a bare token and ld rejects it ("nonconstant expression for
 * subsection alignment"). struct hid_viz_cap's strictest member is a pointer, so
 * 4-byte alignment (32-bit ARM) is correct.
 */
ITERABLE_SECTION_ROM(hid_viz_cap, 4)
