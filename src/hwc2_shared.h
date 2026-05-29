#pragma once
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
/* Shared HWC2 state - set by EGL wrapper, used by DRM shim */
extern hwc2_compat_display_t *shared_hwc2_disp;
extern hwc2_compat_layer_t   *shared_hwc2_layer;
void shared_hwc2_present(struct ANativeWindowBuffer *nb);
