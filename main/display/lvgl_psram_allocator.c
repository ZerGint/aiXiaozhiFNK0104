// LVGL heap in PSRAM (LV_STDLIB_CUSTOM).
//
// With the C library allocator every LVGL object, style and timer at or below
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes lands in internal RAM, which ends
// up holding thousands of small long-lived blocks. LVGL only touches this
// memory from its own task, never from ISRs or while the flash cache is
// disabled, so it can live in PSRAM. DMA draw buffers are allocated by
// esp_lvgl_port with explicit caps and are not affected.
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>
#include <stdlib.h>

#define LVGL_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, LVGL_HEAP_CAPS);
    return p != NULL ? p : malloc(size);
}

void* lv_realloc_core(void* p, size_t new_size) {
    void* r = heap_caps_realloc(p, new_size, LVGL_HEAP_CAPS);
    return r != NULL || new_size == 0 ? r : realloc(p, new_size);
}

void lv_free_core(void* p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
