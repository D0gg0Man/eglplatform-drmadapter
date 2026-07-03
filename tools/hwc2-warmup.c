/* hwc2-warmup: throwaway first composer client.
 *
 * The FIRST HWC2 composer client after the HAL starts (cold boot / HAL restart)
 * gets a mis-initialised display pipeline: presents intermittently reach the
 * glass as black frames (rapid flicker under motion, sometimes a blank
 * session). Any client that starts AFTER a previous client has actually
 * PRESENTED frames is clean. So before the real compositor starts, connect,
 * present a few black frames through the full validate/target/present path,
 * and disconnect. The real client then starts in the good state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/nativebase/nativebase.h>
#include <android/system/window.h>
#include <hybris/gralloc/gralloc.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>

static void on_hotplug(HWC2EventListener *l, int32_t s, hwc2_display_t d, bool c, bool p) {}
static void on_vsync(HWC2EventListener *l, int32_t s, hwc2_display_t d, int64_t t) {}
static void on_refresh(HWC2EventListener *l, int32_t s, hwc2_display_t d) {}

static void nb_incref(struct android_native_base_t *b) {}
static void nb_decref(struct android_native_base_t *b) {}

int main(void) {
    hwc2_compat_device_t *dev = hwc2_compat_device_new(false);
    if (!dev) { fprintf(stderr, "hwc2-warmup: device_new failed\n"); return 1; }
    static HWC2EventListener listener = {
        .on_vsync_received = on_vsync,
        .on_hotplug_received = on_hotplug,
        .on_refresh_received = on_refresh,
    };
    hwc2_compat_device_register_callback(dev, &listener, 0);
    hwc2_compat_device_on_hotplug(dev, 0, true);
    hwc2_compat_display_t *disp = hwc2_compat_device_get_display_by_id(dev, 0);
    if (!disp) { fprintf(stderr, "hwc2-warmup: no display\n"); return 1; }
    HWC2DisplayConfig *cfg = hwc2_compat_display_get_active_config(disp);
    if (!cfg || cfg->width <= 0 || cfg->height <= 0) {
        fprintf(stderr, "hwc2-warmup: no active config\n"); return 1;
    }
    int w = cfg->width, h = cfg->height;
    hwc2_compat_display_set_power_mode(disp, 2 /* ON */);
    hwc2_compat_display_set_vsync_enabled(disp, 1);

    hwc2_compat_layer_t *layer = hwc2_compat_display_create_layer(disp);
    hwc2_compat_layer_set_composition_type(layer, 4 /* CLIENT */);
    hwc2_compat_layer_set_blend_mode(layer, 1 /* NONE */);
    hwc2_compat_layer_set_source_crop(layer, 0, 0, w, h);
    hwc2_compat_layer_set_display_frame(layer, 0, 0, w, h);
    hwc2_compat_layer_set_visible_region(layer, 0, 0, w, h);

    /* Allocate a black gralloc buffer and wrap it as an ANativeWindowBuffer. */
    hybris_gralloc_initialize(0);
    buffer_handle_t handle = NULL;
    uint32_t stride = 0;
    /* usage: HW_FB(0x1000) | HW_RENDER(0x200) | HW_COMPOSER(0x800) | SW_WRITE_RARELY(0x20) */
    if (hybris_gralloc_allocate(w, h, 1 /* RGBA_8888 */, 0x1000 | 0x200 | 0x800 | 0x20,
                                &handle, &stride) != 0 || !handle) {
        fprintf(stderr, "hwc2-warmup: gralloc allocate failed\n"); return 1;
    }
    void *vaddr = NULL;
    if (hybris_gralloc_lock(handle, 0x20 | 0x3, 0, 0, w, h, &vaddr) == 0 && vaddr) {
        memset(vaddr, 0, (size_t)stride * h * 4);
        hybris_gralloc_unlock(handle);
    }
    static ANativeWindowBuffer anwb;
    memset(&anwb, 0, sizeof anwb);
    anwb.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    anwb.common.version = sizeof(ANativeWindowBuffer);
    anwb.common.incRef = nb_incref;
    anwb.common.decRef = nb_decref;
    anwb.width = w; anwb.height = h;
    anwb.stride = stride; anwb.format = 1; anwb.usage = 0x1000 | 0x200 | 0x800;
    anwb.handle = handle;

    for (int i = 0; i < 3; i++) {
        uint32_t nt = 0, nr = 0;
        int vr = hwc2_compat_display_validate(disp, &nt, &nr);
        if (vr == 5 /* HAS_CHANGES */)
            hwc2_compat_display_accept_changes(disp);
        hwc2_compat_display_set_client_target(disp, 0, &anwb, -1, 0);
        int32_t fence = -1;
        int pr = hwc2_compat_display_present(disp, &fence);
        fprintf(stderr, "hwc2-warmup: frame %d validate=%d nt=%u nr=%u present=%d\n",
                i, vr, nt, nr, pr);
        if (fence >= 0) close(fence);
        usleep(20 * 1000);
    }
    hybris_gralloc_release(handle, 1);
    fprintf(stderr, "hwc2-warmup: done\n");
    return 0;
}
