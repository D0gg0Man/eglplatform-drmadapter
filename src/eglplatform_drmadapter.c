/*
 * eglplatform_drmadapter.c - hybris EGL platform for GNOME Shell on Mali/HWC2
 *
 * Set HYBRIS_EGLPLATFORM=drmadapter to use this platform.
 * Handles HWC2 init and present. Exposes hwc2 state for GLVND vendor wrapper.
 */

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hybris/eglplatformcommon/ws.h>
#include <hybris/eglplatformcommon/eglplatformcommon.h>
#include <hybris/gralloc/gralloc.h>
#include <hardware/gralloc.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#define LOG(fmt, ...) g_debug("drmadapter: " fmt, ##__VA_ARGS__)

/* Panel dimensions are queried from HWC2 at init (never hardcoded). */
static int frame_w = 0, frame_h = 0;

/* Env-gated tracing (DRMADAPTER_TRACE=1) for diagnosing the HWC2 present
 * path; no-op unless enabled. Writes to /tmp/drmadapter-trace.log. */
#include <stdarg.h>
static int dtrace = -1;
static void dtracef(const char *fmt, ...) {
    if (dtrace < 0) dtrace = getenv("DRMADAPTER_TRACE") ? 1 : 0;
    if (!dtrace) return;
    FILE *f = fopen("/tmp/drmadapter-trace.log", "a");
    if (f) { va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f); }
}

static hwc2_compat_device_t  *hwc2_dev   = NULL;
static hwc2_compat_display_t *hwc2_disp  = NULL;
static hwc2_compat_layer_t   *hwc2_layer = NULL;
static int hwc2_ready = 0;
static pthread_mutex_t hwc2_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Exported getters for GLVND vendor wrapper */
hwc2_compat_display_t *drmadapter_get_hwc2_display(void) { return hwc2_disp; }
hwc2_compat_layer_t   *drmadapter_get_hwc2_layer(void)   { return hwc2_layer; }

static void on_hotplug(HWC2EventListener *s,int32_t q,hwc2_display_t d,bool c,bool p){}
static void on_vsync(HWC2EventListener *s,int32_t q,hwc2_display_t d,int64_t t){
    /* Feed real panel vsync timestamps to libdrm-hybris so it phase-aligns the
     * synthetic flip deadlines to the actual vblank (kills the present/latch
     * beat that showed as motion flicker with session-random severity). */
    static void (*stamp)(int64_t) = NULL;
    static int resolved = 0;
    if (!resolved) { stamp = (void(*)(int64_t))dlsym(RTLD_DEFAULT, "drm_shim_vsync_stamp"); resolved = 1; }
    if (stamp) stamp(t);
}
static void on_refresh(HWC2EventListener *s,int32_t q,hwc2_display_t d){}

static void init_hwc2(void) {
    pthread_mutex_lock(&hwc2_mutex);
    if (hwc2_ready) { pthread_mutex_unlock(&hwc2_mutex); return; }
    LOG("init_hwc2: starting");
    hybris_gralloc_initialize(0);
    hwc2_dev = hwc2_compat_device_new(false);
    if (!hwc2_dev) { LOG("device_new failed"); goto out; }
    HWC2EventListener *l = calloc(1, sizeof(*l));
    l->on_vsync_received   = on_vsync;
    l->on_hotplug_received = on_hotplug;
    l->on_refresh_received = on_refresh;
    hwc2_compat_device_register_callback(hwc2_dev, l, 0);
    hwc2_compat_device_on_hotplug(hwc2_dev, 0, true);
    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev, 0);
    if (!hwc2_disp) { LOG("no display"); goto out; }
    /* Query the real panel geometry from HWC2 rather than hardcoding it. */
    HWC2DisplayConfig *cfg = hwc2_compat_display_get_active_config(hwc2_disp);
    if (cfg && cfg->width > 0 && cfg->height > 0) {
        frame_w = cfg->width; frame_h = cfg->height;
        LOG("panel %dx%d vsync=%lldns dpi=%.0fx%.0f", frame_w, frame_h,
            (long long)cfg->vsyncPeriod, (double)cfg->dpiX, (double)cfg->dpiY);
        /* Pace libdrm-hybris's synthetic flip events at the real panel vsync. */
        void (*set_vsync)(uint64_t) = (void(*)(uint64_t))dlsym(RTLD_DEFAULT, "drm_shim_set_vsync_period");
        if (set_vsync && cfg->vsyncPeriod > 0) set_vsync((uint64_t)cfg->vsyncPeriod);
    } else {
        LOG("ERROR: HWC2 active config unavailable; cannot determine panel size");
        goto out;
    }
    hwc2_compat_display_set_power_mode(hwc2_disp, 2);
    hwc2_compat_display_set_vsync_enabled(hwc2_disp, 1);
    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);
    hwc2_compat_layer_set_composition_type(hwc2_layer, 4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer, 1);
    hwc2_compat_layer_set_source_crop(hwc2_layer, 0, 0, frame_w, frame_h);
    hwc2_compat_layer_set_display_frame(hwc2_layer, 0, 0, frame_w, frame_h);
    hwc2_compat_layer_set_visible_region(hwc2_layer, 0, 0, frame_w, frame_h);
    hwc2_ready = 1;
    LOG("init_hwc2: done disp=%p layer=%p", (void*)hwc2_disp, (void*)hwc2_layer);
out:
    pthread_mutex_unlock(&hwc2_mutex);
}

static int drmadapter_present_gralloc(buffer_handle_t handle);
static int drmadapter_present_cpu(const void *src, uint32_t src_pitch);

/* DPMS: libdrm-hybris calls this when wlroots toggles the CRTC ACTIVE state, so
 * we power the real panel down on blank and back up on wake (HWC2 power modes:
 * 0=OFF, 1=DOZE, 2=ON). */
/* Path to the panel backlight. HWC2 set_power_mode(OFF) reliably wedges phoc's
 * faked-KMS present loop (input dies, frame clock stalls, never recovers), so we
 * "blank" softly instead: drop the backlight brightness and leave HWC2 powered
 * on. The compositor then just idles like any static screen (which never freezes)
 * and wake is simply restoring the brightness. Override the path with
 * DRMADAPTER_BACKLIGHT if the device differs. */
static const char *backlight_path(void) {
    static const char *p = NULL;
    if (!p) {
        p = getenv("DRMADAPTER_BACKLIGHT");
        if (!p || !*p) p = "/sys/class/leds/lcd-backlight/brightness";
    }
    return p;
}
static int backlight_read(void) {
    FILE *f = fopen(backlight_path(), "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}
static void backlight_write(int v) {
    FILE *f = fopen(backlight_path(), "w");
    if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}

static int g_saved_brightness = -1;

static int g_backlight_owner = -1;  /* -1 unknown; 1 = we own it; 0 = phosh/gsd owns it */

static void drmadapter_set_power(int on) {
    if (g_backlight_owner < 0) {
        /* DRMADAPTER_BACKLIGHT_OWNER=1 -> we drive the backlight on DPMS.
         * default (0) -> leave it to phosh/gsd; we only avoid the HWC2 power
         * toggle that wedges the present loop. */
        const char *e = getenv("DRMADAPTER_BACKLIGHT_OWNER");
        g_backlight_owner = (e && *e == '1') ? 1 : 0;
    }
    if (g_backlight_owner) {
        if (on) {
            int v = g_saved_brightness;
            if (v <= 0) {
                FILE *f = fopen("/sys/class/leds/lcd-backlight/max_brightness", "r");
                int mx = 0;
                if (f) { if (fscanf(f, "%d", &mx) != 1) mx = 0; fclose(f); }
                v = mx > 0 ? mx / 2 : 1;
            }
            backlight_write(v);
        } else {
            int cur = backlight_read();
            if (cur > 0)
                g_saved_brightness = cur;
            backlight_write(0);
        }
    }
    /* Real HWC2 DPMS (actual panel off), gated on DRMADAPTER_HWC2_DPMS=1. This
     * historically wedged the faked-KMS frame clock on the off->on cycle; now
     * that libdrm-hybris's eventfd flip-wake delivers synth flips promptly even
     * from an idle loop, re-test whether the power cycle survives. Off by default
     * (backlight-only "blank" via phosh). */
    {
        static int hwc2_dpms = -1;
        if (hwc2_dpms < 0) { const char *e = getenv("DRMADAPTER_HWC2_DPMS"); hwc2_dpms = (e && *e == '1') ? 1 : 0; }
        if (hwc2_dpms) {
            pthread_mutex_lock(&hwc2_mutex);
            if (hwc2_ready && hwc2_disp) {
                hwc2_compat_display_set_power_mode(hwc2_disp, on ? 2 : 0);
                if (on) hwc2_compat_display_set_vsync_enabled(hwc2_disp, 1);
            }
            pthread_mutex_unlock(&hwc2_mutex);
        }
    }
    LOG("set_power -> %s (owner=%d)", on ? "ON" : "OFF", g_backlight_owner);
}

static void drmadapterws_init_module(struct ws_egl_interface *egl_iface) {
    LOG("init_module");
    eglplatformcommon_init(egl_iface);
    setenv("EGL_PLATFORM", "hwcomposer", 0);
    /* Register our wlroots present callback into the globally-preloaded
     * libdrm-hybris shim (reachable via RTLD_DEFAULT; this ws module is not). */
    void (*set_present)(int (*)(buffer_handle_t)) =
        (void (*)(int (*)(buffer_handle_t)))dlsym(RTLD_DEFAULT, "drm_shim_set_present");
    if (set_present) {
        set_present(drmadapter_present_gralloc);
        LOG("registered drmadapter_present_gralloc with libdrm-hybris");
    }
    /* Single-pass CPU present for the QPainter (software) compositor path. */
    void (*set_present_cpu)(int (*)(const void *, uint32_t)) =
        (void (*)(int (*)(const void *, uint32_t)))dlsym(RTLD_DEFAULT, "drm_shim_set_present_cpu");
    if (set_present_cpu) {
        set_present_cpu(drmadapter_present_cpu);
        LOG("registered drmadapter_present_cpu with libdrm-hybris");
    }
    /* Same for the DPMS power callback. */
    void (*set_power)(void (*)(int)) =
        (void (*)(void (*)(int)))dlsym(RTLD_DEFAULT, "drm_shim_set_power");
    if (set_power) {
        set_power(drmadapter_set_power);
        LOG("registered drmadapter_set_power with libdrm-hybris");
    }
    pthread_t t;
    pthread_create(&t, NULL, (void*(*)(void*))init_hwc2, NULL);
    pthread_detach(t);
}

static struct _EGLDisplay *drmadapterws_GetDisplay(EGLNativeDisplayType native) {
    LOG("GetDisplay native=%p", (void*)native);
    struct _EGLDisplay *dpy = calloc(1, sizeof(*dpy));
    dpy->display_id = EGL_DEFAULT_DISPLAY;
    dpy->dpy = EGL_NO_DISPLAY;
    return dpy;
}

static void drmadapterws_releaseDisplay(struct _EGLDisplay *dpy) {
    LOG("releaseDisplay");
    free(dpy);
}

static void drmadapterws_Terminate(struct _EGLDisplay *dpy) {
    LOG("Terminate");
}

static void drmadapterws_eglInitialized(struct _EGLDisplay *dpy) {
    LOG("eglInitialized dpy=%p", (void*)dpy);
}

static void present_cb(void *ud, struct ANativeWindow *w, struct ANativeWindowBuffer *buf) {
    static unsigned long pn = 0;
    pn++;
    /* The buffer arrives carrying Mali's render-complete (acquire) fence from
     * queueBuffer. It MUST be closed every frame -- leaking it exhausts the
     * process fd table after ~900 frames (915 stranded sync_files at the 1024
     * limit) and then EVERYTHING needing an fd fails: presents stop (screen
     * freezes stale), new wayland clients can't connect, Xwayland dies. Do NOT
     * hand it to HWC2 as the acquire fence though: on this faked-KMS pipeline
     * that fence doesn't reliably signal for HWC2 and it scans out NOTHING
     * (pure black panel while present() returns 0). Rendering completion is
     * already guaranteed upstream (glFinish in the wlroots render submit), so
     * present without an acquire fence -- the long-proven visible path. */
    if (buf) {
        int acquire = HWCNativeBufferGetFence(buf);
        if (acquire >= 0) close(acquire);
        HWCNativeBufferSetFence(buf, -1); /* clear slot so the window won't also close it */
    }
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || !buf) {
        dtracef("present_cb #%lu SKIP ready=%d disp=%p layer=%p buf=%p\n",
                pn, hwc2_ready, (void*)hwc2_disp, (void*)hwc2_layer, (void*)buf);
        return;
    }
    uint32_t nt = 0, nr = 0;
    int vr = hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    /* Mirror wlroots' hwcomposer2 backend EXACTLY (it drives this same panel
     * flicker-free under stock phosh): the layer stays CLIENT composition with
     * NO buffer -- the client target alone carries the frame -- and frames where
     * the composer demands composition-type changes are NOT presented. We used
     * to accept_changes and present anyway; the composer then device-composited
     * our bufferless layer and the glass got a BLACK frame -> rapid black
     * flashes on every burst of presents (motion), occasionally a whole blank
     * session. Skipping those frames just re-shows the previous good frame. */
    if (vr != 0 /*HWC2_ERROR_NONE*/ && vr != 5 /*HWC2_ERROR_HAS_CHANGES*/) {
        dtracef("present_cb #%lu validate failed %d\n", pn, vr);
        return;
    }
    /* MTK HAL quirk: the composition-type-change demand can arrive with
     * vr=NONE and nt>0 (not HAS_CHANGES). Accept WHENEVER types are pending --
     * leaving the demand unaccepted puts the composer in a mismatched state and
     * presents intermittently reach the glass as BLACK (the flicker/blank
     * lottery). Always present afterwards; skipping frames is worse here. */
    if (nt)
        hwc2_compat_display_accept_changes(hwc2_disp);
    /* Give each distinct window buffer a STABLE slot. The HAL caches buffers
     * per slot; recycling slot 0 for all three cycling buffers thrashes the
     * cache and (depending on per-boot HAL state) it sometimes latches a stale
     * cached buffer -> black/old flashes on presents. SurfaceFlinger does the
     * same per-buffer slot assignment. */
    uint32_t slot = 0;
    {
        static struct ANativeWindowBuffer *slot_map[8];
        static int slot_n = 0;
        int found = -1;
        for (int i = 0; i < slot_n; i++)
            if (slot_map[i] == buf) { found = i; break; }
        if (found < 0 && slot_n < 8) { slot_map[slot_n] = buf; found = slot_n++; }
        if (found >= 0) slot = (uint32_t)found;
    }
    /* Keep the LAYER buffer set as well as the client target: if the composer
     * negotiates the layer to DEVICE composition (per-session lottery, accepted
     * above), it scans the LAYER buffer directly -- with no buffer there the
     * glass stays permanently black while presents "succeed" (the blank-session
     * state). With both set, whichever composition mode wins shows our frame. */
    hwc2_compat_layer_set_buffer(hwc2_layer, slot, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, slot, buf, -1, 0);
    int32_t fence = -1;
    int pr = hwc2_compat_display_present(hwc2_disp, &fence);
    dtracef("present_cb #%lu validate=%d nt=%u nr=%u present=%d fence=%d buf=%p\n",
            pn, vr, nt, nr, pr, fence, (void*)buf);
    if (fence >= 0) close(fence);
}

/* wlroots presentation path.
 *
 * wlroots renders into a gbm_hybris (gralloc) buffer and scans it out with an
 * atomic KMS commit. There is no drmadapter EGL window surface in that path, so
 * present_cb (driven by mutter's eglSwapBuffers) never runs. libdrm-hybris's
 * faked KMS commit instead calls back here with the committed gralloc handle;
 * we look up the RemoteWindowBuffer libhybris already built for it (when
 * wlroots imported it as an EGL image) and present it through the same HWC2
 * display/layer. Registered into the globally-preloaded libdrm-hybris at init,
 * because the ws module is dlopen()'d RTLD_LAZY (local) and is not otherwise
 * reachable by the shim. */
extern void *eglplatformcommon_wlr_lookup_anwb(buffer_handle_t handle);
extern void *eglplatformcommon_wlr_make_anwb(buffer_handle_t handle);
extern void *eglplatformcommon_wlr_display(void);

/* Present path: render the committed wlroots buffer into OUR OWN linear
 * gralloc buffers via FBO, and hand those to HWC2 directly (validate/accept/
 * set-layer-buffer/set-client-target/present) -- the exact flow a CPU-written
 * test buffer was PROVEN to display. The previous HWCNativeWindow/EGL-swapchain
 * path was subject to a per-allocation lottery (buffers whose GL writes never
 * reached the glass: black flashes under motion / blank sessions). No window,
 * no swapchain, no lottery. */
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#define BL_NBUF 3
static EGLDisplay   bl_dpy  = EGL_NO_DISPLAY;
static EGLContext   bl_ctx  = EGL_NO_CONTEXT;
static EGLSurface   bl_surf = EGL_NO_SURFACE;  /* 1x1 pbuffer, makecurrent target only */
static GLuint       bl_prog = 0, bl_pos = 0;
static int          bl_ready = 0, bl_failed = 0;
static PFNEGLCREATEIMAGEKHRPROC  p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

static void bl_nb_incref(struct android_native_base_t *b) { (void)b; }
static void bl_nb_decref(struct android_native_base_t *b) { (void)b; }

/* Our presentation buffers: linear gralloc allocations wrapped as
 * ANativeWindowBuffers, each with an EGLImage-backed FBO to render into. */
static struct {
    buffer_handle_t handle;
    uint32_t stride;
    ANativeWindowBuffer anwb;
    EGLImageKHR img;
    GLuint tex, fbo;
} bl_buf[BL_NBUF];
static int bl_cur = 0;
static int bl_last_present_fence = -1;
/* Serializes HWC2 validate/set/present across threads: the QPainter present
 * worker and the wlroots gralloc path may otherwise submit concurrently. */
static pthread_mutex_t hwc2_present_mu = PTHREAD_MUTEX_INITIALIZER;

static GLuint bl_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOG("blit shader err: %s", log); }
    return s;
}

/* Allocate + import one presentation buffer; returns 0 on failure. */
static int bl_buf_init(int i) {
    /* usage 0x1b33: HW_FB|HW_COMPOSER|HW_RENDER|SW_READ_OFTEN|SW_WRITE_OFTEN --
     * the exact combination proven to present correctly (CPU gray test). */
    const uint64_t usage = 0x1000 | 0x800 | 0x200 | 0x33;
    if (hybris_gralloc_allocate(frame_w, frame_h, 1 /*RGBA_8888*/, usage,
                                &bl_buf[i].handle, &bl_buf[i].stride) != 0 || !bl_buf[i].handle) {
        fprintf(stderr, "drmadapter: present buffer %d: gralloc allocate failed\n", i);
        return 0;
    }
    ANativeWindowBuffer *nb = &bl_buf[i].anwb;
    memset(nb, 0, sizeof *nb);
    nb->common.magic   = ANDROID_NATIVE_BUFFER_MAGIC;
    nb->common.version = sizeof(ANativeWindowBuffer);
    nb->common.incRef  = bl_nb_incref;
    nb->common.decRef  = bl_nb_decref;
    nb->width  = frame_w;
    nb->height = frame_h;
    nb->stride = bl_buf[i].stride;
    nb->format = 1; /* RGBA_8888, matches the allocation */
    nb->usage  = usage;
    nb->handle = bl_buf[i].handle;

    bl_buf[i].img = p_eglCreateImageKHR(bl_dpy, EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)nb, NULL);
    if (bl_buf[i].img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "drmadapter: present buffer %d: eglCreateImageKHR failed 0x%x\n", i, eglGetError());
        return 0;
    }
    glGenTextures(1, &bl_buf[i].tex);
    glBindTexture(GL_TEXTURE_2D, bl_buf[i].tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)bl_buf[i].img);
    glGenFramebuffers(1, &bl_buf[i].fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, bl_buf[i].fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bl_buf[i].tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "drmadapter: present buffer %d: FBO incomplete 0x%x\n", i, st);
        return 0;
    }
    return 1;
}

static int blit_init(void) {
    if (bl_ready) return 1;
    if (bl_failed) return 0;
    /* Use the SAME EGLDisplay handle wlroots imports its buffers on, captured by
     * eglplatformcommon -- a different handle makes the buffer re-import fail. */
    bl_dpy = (EGLDisplay)eglplatformcommon_wlr_display();
    if (bl_dpy == EGL_NO_DISPLAY) bl_dpy = eglGetCurrentDisplay();
    if (bl_dpy == EGL_NO_DISPLAY) bl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (bl_dpy == EGL_NO_DISPLAY) { bl_failed = 1; return 0; }
    LOG("blit: using wlr EGLDisplay %p", (void*)bl_dpy);

    EGLint cfgattr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(bl_dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        LOG("blit: no pbuffer EGL config"); bl_failed = 1; return 0;
    }
    EGLint pba[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    bl_surf = eglCreatePbufferSurface(bl_dpy, cfg, pba);
    if (bl_surf == EGL_NO_SURFACE) { LOG("blit: pbuffer failed 0x%x", eglGetError()); bl_failed = 1; return 0; }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    bl_ctx = eglCreateContext(bl_dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (bl_ctx == EGL_NO_CONTEXT) { LOG("blit: context failed 0x%x", eglGetError()); bl_failed = 1; return 0; }

    p_eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) { LOG("blit: missing procs"); bl_failed = 1; return 0; }

    EGLContext sc = eglGetCurrentContext();
    EGLSurface sd = eglGetCurrentSurface(EGL_DRAW), sr = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(bl_dpy, bl_surf, bl_surf, bl_ctx)) {
        LOG("blit: makecurrent failed 0x%x", eglGetError()); bl_failed = 1; return 0;
    }

    static const char *vs =
        "attribute vec2 pos;\nvarying vec2 uv;\n"
        "void main(){ uv = vec2((pos.x+1.0)*0.5, (pos.y+1.0)*0.5); gl_Position = vec4(pos,0.0,1.0); }\n";
    static const char *fs =
        "precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\n"
        "void main(){ gl_FragColor = texture2D(tex, uv); }\n";
    bl_prog = glCreateProgram();
    glAttachShader(bl_prog, bl_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(bl_prog, bl_shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(bl_prog, 0, "pos");
    glLinkProgram(bl_prog);
    bl_pos = 0;

    for (int i = 0; i < BL_NBUF; i++) {
        if (!bl_buf_init(i)) {
            eglMakeCurrent(bl_dpy, sd, sr, sc);
            bl_failed = 1; return 0;
        }
    }

    /* End-to-end validation using the REAL runtime path: draw a textured quad
     * (the same shader the blit uses) into each buffer and verify through the
     * CPU (gralloc lock) -- the CPU view is what the display controller scans.
     * A per-allocation lottery can hand out buffers where plain clears land but
     * SHADER writes never reach memory (the gray-glass boots); when a buffer
     * fails, free and reallocate ALL of them and try again. */
    {
        GLuint vtex = 0;
        static const unsigned char graytex[4] = { 96, 96, 96, 255 };
        glGenTextures(1, &vtex);
        glBindTexture(GL_TEXTURE_2D, vtex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, graytex);
        int attempt;
        for (attempt = 0; attempt < 8; attempt++) {
            int bad = 0;
            for (int i = 0; i < BL_NBUF; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, bl_buf[i].fbo);
                glViewport(0, 0, frame_w, frame_h);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(bl_prog);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, vtex);
                glUniform1i(glGetUniformLocation(bl_prog, "tex"), 0);
                static const GLfloat q[] = { -1,-1, 1,-1, -1,1, 1,1 };
                glEnableVertexAttribArray(bl_pos);
                glVertexAttribPointer(bl_pos, 2, GL_FLOAT, GL_FALSE, 0, q);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glDisableVertexAttribArray(bl_pos);
                glFinish();
                int cpu_ok = 0;
                void *va = NULL;
                if (hybris_gralloc_lock(bl_buf[i].handle, 0x3, 0, 0, frame_w, frame_h, &va) == 0 && va) {
                    unsigned char *p2 = (unsigned char*)va +
                        (size_t)(frame_h/2) * bl_buf[i].stride * 4 + (size_t)(frame_w/2) * 4;
                    cpu_ok = (p2[0] >= 80 && p2[0] <= 112);
                    hybris_gralloc_unlock(bl_buf[i].handle);
                }
                if (!cpu_ok) bad++;
            }
            if (bad == 0) break;
            fprintf(stderr, "drmadapter: %d/%d present buffers fail shader-path validation, reallocating (attempt %d)\n",
                    bad, BL_NBUF, attempt + 1);
            for (int i = 0; i < BL_NBUF; i++) {
                glDeleteFramebuffers(1, &bl_buf[i].fbo);
                glDeleteTextures(1, &bl_buf[i].tex);
                if (bl_buf[i].img != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(bl_dpy, bl_buf[i].img);
                if (bl_buf[i].handle) hybris_gralloc_release(bl_buf[i].handle, 1);
                memset(&bl_buf[i], 0, sizeof bl_buf[i]);
            }
            int fail = 0;
            for (int i = 0; i < BL_NBUF; i++)
                if (!bl_buf_init(i)) { fail = 1; break; }
            if (fail) { eglMakeCurrent(bl_dpy, sd, sr, sc); bl_failed = 1; return 0; }
        }
        glDeleteTextures(1, &vtex);
        if (attempt >= 8) {
            fprintf(stderr, "drmadapter: present buffers never validated; presents disabled\n");
            eglMakeCurrent(bl_dpy, sd, sr, sc);
            bl_failed = 1; return 0;
        }
        fprintf(stderr, "drmadapter: present buffers validated (shader path, %d realloc%s)\n",
                attempt, attempt == 1 ? "" : "s");
    }

    eglMakeCurrent(bl_dpy, sd, sr, sc);
    bl_ready = 1;
    LOG("blit: initialised (%dx%d, %d linear present buffers)", frame_w, frame_h, BL_NBUF);
    return 1;
}

/* Per-handle cache of the imported EGLImage + GL texture for the blit source.
 * Mali allows only ONE import per underlying buffer (a second eglCreateImageKHR
 * fails EGL_BAD_PARAMETER), so the image is imported once and re-bound to the
 * texture each frame (the rebind refreshes the texture's view of the live
 * buffer memory). Must be called with the blit context current. */
#define BLIT_CACHE_MAX 8
/* Per-source-handle cache of the imported EGLImage + texture. The image is
 * imported once (Mali allows one live import per buffer) and re-targeted to the
 * texture each frame to refresh its view of the live buffer memory. */
static struct { buffer_handle_t handle; EGLImageKHR img; GLuint tex; } bl_cache[BLIT_CACHE_MAX];
static int bl_cache_n = 0;
static GLuint blit_tex_for_handle(buffer_handle_t handle) {
    for (int i = 0; i < bl_cache_n; i++)
        if (bl_cache[i].handle == handle) {
            glBindTexture(GL_TEXTURE_2D, bl_cache[i].tex);
            p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)bl_cache[i].img);
            (void)glGetError();
            return bl_cache[i].tex;
        }
    if (bl_cache_n >= BLIT_CACHE_MAX) return 0;
    struct ANativeWindowBuffer *anwb =
        (struct ANativeWindowBuffer *)eglplatformcommon_wlr_make_anwb(handle);
    if (!anwb) return 0;
    EGLImageKHR img = p_eglCreateImageKHR(bl_dpy, EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)anwb, NULL);
    if (img == EGL_NO_IMAGE_KHR) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    bl_cache[bl_cache_n].handle = handle;
    bl_cache[bl_cache_n].img = img;
    bl_cache[bl_cache_n].tex = tex;
    bl_cache_n++;
    return tex;
}

static int drmadapter_present_gralloc(buffer_handle_t handle) {
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || frame_w <= 0) return -1;
    if (!handle) return -1;
    if (!blit_init()) return -1;

    /* Pace to the display: wait for the PREVIOUS present's fence before
     * rendering/presenting the next frame. Without this we cycle the 3 present
     * buffers faster than the composer latches and re-render into a buffer it
     * is still scanning -> out-of-order frames (jitter/tearing under motion). */
    if (bl_last_present_fence >= 0) {
        struct pollfd pf = { .fd = bl_last_present_fence, .events = POLLIN };
        poll(&pf, 1, 100);
        close(bl_last_present_fence);
        bl_last_present_fence = -1;
    }

    /* Save wlroots' current EGL state (frequently nothing -- it unbinds after
     * rendering), switch to the blitter, render, present. Restore via bl_dpy. */
    EGLContext sc = eglGetCurrentContext();
    EGLSurface sd = eglGetCurrentSurface(EGL_DRAW), sr = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(bl_dpy, bl_surf, bl_surf, bl_ctx)) {
        static int w = 0; if (!w) { LOG("blit: makecurrent failed 0x%x", eglGetError()); w = 1; }
        return -1;
    }

    /* DEFAULT: copy the (linear) source buffer into the present buffer with
     * the CPU -- no GL sampling at all. Cross-context EGLImage sampling of the
     * source was the last unreliable link (stale snapshots = ghosting/trailing
     * under motion, per-boot severity); the CPU copy is byte-exact and
     * deterministic, and comfortably fast for linear buffers. Set
     * DRMADAPTER_GL_BLIT=1 to use the GL sampling path instead. */
    {
        static int cpu_copy = -1;
        if (cpu_copy < 0) { const char *e = getenv("DRMADAPTER_GL_BLIT"); cpu_copy = (e && *e == '1') ? 0 : 1; }
        if (cpu_copy) {
            int i2 = bl_cur;
            bl_cur = (bl_cur + 1) % BL_NBUF;
            void *sv = NULL, *dv = NULL;
            if (hybris_gralloc_lock(handle, 0x3, 0, 0, frame_w, frame_h, &sv) == 0 && sv) {
                if (hybris_gralloc_lock(bl_buf[i2].handle, 0x30, 0, 0, frame_w, frame_h, &dv) == 0 && dv) {
                    size_t spitch = (size_t)bl_buf[i2].stride * 4; /* same allocator/params */
                    /* Copy with a red/blue channel swap: wlroots' buffer holds
                     * XRGB little-endian (B,G,R,X bytes) while the present
                     * buffer is scanned as RGBA -- a raw copy shows swapped
                     * colors. The word swizzle auto-vectorizes to NEON. */
                    for (int y = 0; y < frame_h; y++) {
                        const uint32_t *sp = (const uint32_t *)((const char*)sv + (size_t)y * spitch);
                        uint32_t *dp = (uint32_t *)((char*)dv + (size_t)y * spitch);
                        for (int x = 0; x < frame_w; x++) {
                            uint32_t v = sp[x];
                            dp[x] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
                        }
                    }
                    hybris_gralloc_unlock(bl_buf[i2].handle);
                }
                hybris_gralloc_unlock(handle);
            }
            eglMakeCurrent(bl_dpy, sd, sr, sc);
            pthread_mutex_lock(&hwc2_present_mu);
            uint32_t nt2 = 0, nr2 = 0;
            int vr2 = hwc2_compat_display_validate(hwc2_disp, &nt2, &nr2);
            if (vr2 != 0 && vr2 != 5) { pthread_mutex_unlock(&hwc2_present_mu); return -1; }
            if (nt2) hwc2_compat_display_accept_changes(hwc2_disp);
            hwc2_compat_layer_set_buffer(hwc2_layer, (uint32_t)i2, &bl_buf[i2].anwb, -1);
            hwc2_compat_display_set_client_target(hwc2_disp, (uint32_t)i2, &bl_buf[i2].anwb, -1, 0);
            int32_t f2 = -1;
            hwc2_compat_display_present(hwc2_disp, &f2);
            pthread_mutex_unlock(&hwc2_present_mu);
            if (f2 >= 0) close(f2);
            static int hb2 = -2; static const char *hbp2;
            if (hb2 == -2) { hbp2 = getenv("DRMADAPTER_HEARTBEAT"); hb2 = hbp2 ? 1 : 0; }
            static unsigned long okn2 = 0;
            if (hb2 && ((okn2++ % 3) == 0)) {
                FILE *f = fopen(hbp2, "w");
                if (f) { fprintf(f, "%lu\n", okn2); fclose(f); }
            }
            return 0;
        }
    }

    GLuint tex = blit_tex_for_handle(handle);
    if (tex == 0) {
        static unsigned long z = 0;
        if ((z++ % 300) == 0)
            fprintf(stderr, "drmadapter: BLIT FAILED (no source tex) #%lu handle=%p\n", z, (void*)handle);
        eglMakeCurrent(bl_dpy, sd, sr, sc);
        return -1;
    }

    int i = bl_cur;
    bl_cur = (bl_cur + 1) % BL_NBUF;
    glBindFramebuffer(GL_FRAMEBUFFER, bl_buf[i].fbo);
    glViewport(0, 0, frame_w, frame_h);
    glUseProgram(bl_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(bl_prog, "tex"), 0);
    static const GLfloat quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glEnableVertexAttribArray(bl_pos);
    glVertexAttribPointer(bl_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(bl_pos);
    /* Ensure the render fully lands in the (linear) buffer memory before the
     * composer scans it: glFinish completes the GPU work. */
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eglMakeCurrent(bl_dpy, sd, sr, sc); /* restore wlroots state early */

    /* Present: the exact flow proven by the CPU-written gray test. */
    pthread_mutex_lock(&hwc2_present_mu);
    uint32_t nt = 0, nr = 0;
    int vr = hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (vr != 0 && vr != 5) {
        pthread_mutex_unlock(&hwc2_present_mu);
        dtracef("present validate failed %d\n", vr);
        return -1;
    }
    if (nt)
        hwc2_compat_display_accept_changes(hwc2_disp);
    /* Per-buffer slots: the HAL caches buffers per slot; rotating three
     * different buffers through one slot made the cache serve STALE entries ->
     * older frames interleaved on the glass (ghosting/trailing under motion).
     * One slot per buffer keeps the cache always correct. */
    hwc2_compat_layer_set_buffer(hwc2_layer, (uint32_t)i, &bl_buf[i].anwb, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, (uint32_t)i, &bl_buf[i].anwb, -1, 0);
    int32_t fence = -1;
    int pr = hwc2_compat_display_present(hwc2_disp, &fence);
    pthread_mutex_unlock(&hwc2_present_mu);
    /* Keep the fence: the next present waits on it (pacing), then closes it. */
    bl_last_present_fence = fence;

    static unsigned long pn = 0;
    pn++;
    if (pn <= 12)
        fprintf(stderr, "drmadapter: PRESENT #%lu buf=%d vr=%d nt=%u pr=%d\n", pn, i, vr, nt, pr);
    /* Liveness heartbeat for the session watchdog (cheap; off unless env set). */
    {
        static int hb = -2; static const char *hbp;
        if (hb == -2) { hbp = getenv("DRMADAPTER_HEARTBEAT"); hb = hbp ? 1 : 0; }
        static unsigned long okn = 0;
        if (hb && ((okn++ % 3) == 0)) {
            FILE *f = fopen(hbp, "w");
            if (f) { fprintf(f, "%lu\n", okn); fclose(f); }
        }
    }
    return 0;
}

/* Single-pass CPU present for the QPainter (software) compositor path:
 * swizzle-copy the compositor's dumb-buffer CPU mapping straight into a
 * present buffer and hand it to HWC2. Replaces the old dumb -> scratch
 * gralloc -> present-buffer flow (two full-frame passes) with one. Pure CPU:
 * no EGL context switch, no GL.
 *
 * The work runs on a dedicated worker thread: it costs ~4-5ms per fullscreen
 * frame, and doing it inside the compositor's page-flip call pushed each
 * frame past the vblank budget (hard 60fps ceiling, ~40fps in practice). The
 * flip just queues {src,pitch}; the compositor double-buffers its dumb
 * buffers, so the source stays stable until its NEXT flip -- the same
 * contract real KMS scanout relies on. All HWC2 submission is serialized
 * with hwc2_present_mu (the wlroots gralloc path can fire concurrently). */
static int present_cpu_sync(const void *src, uint32_t src_pitch) {
    static int prof = -1;
    if (prof < 0) prof = getenv("DRMADAPTER_PROF") ? 1 : 0;
    struct timespec t0, t1;
    if (prof) clock_gettime(CLOCK_MONOTONIC, &t0);

    int i2 = bl_cur;
    bl_cur = (bl_cur + 1) % BL_NBUF;
    void *dv = NULL;
    if (hybris_gralloc_lock(bl_buf[i2].handle, 0x30, 0, 0, frame_w, frame_h, &dv) != 0 || !dv)
        return -1;
    size_t dpitch = (size_t)bl_buf[i2].stride * 4;
    if (!src_pitch) src_pitch = (uint32_t)frame_w * 4;
    /* Copy with a red/blue channel swap: the compositor's buffer holds XRGB
     * little-endian (B,G,R,X bytes) while the present buffer is scanned as
     * RGBA. The word swizzle auto-vectorizes to NEON. */
    for (int y = 0; y < frame_h; y++) {
        const uint32_t *sp = (const uint32_t *)((const char *)src + (size_t)y * src_pitch);
        uint32_t *dp = (uint32_t *)((char *)dv + (size_t)y * dpitch);
        for (int x = 0; x < frame_w; x++) {
            uint32_t v = sp[x];
            dp[x] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
        }
    }
    hybris_gralloc_unlock(bl_buf[i2].handle);

    pthread_mutex_lock(&hwc2_present_mu);
    uint32_t nt = 0, nr = 0;
    int vr = hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (vr != 0 && vr != 5) {
        pthread_mutex_unlock(&hwc2_present_mu);
        static unsigned long vf = 0;
        if (vf++ < 5) fprintf(stderr, "drmadapter: present_cpu validate failed %d (#%lu)\n", vr, vf);
        return -1;
    }
    if (nt) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, (uint32_t)i2, &bl_buf[i2].anwb, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, (uint32_t)i2, &bl_buf[i2].anwb, -1, 0);
    int32_t f = -1;
    hwc2_compat_display_present(hwc2_disp, &f);
    pthread_mutex_unlock(&hwc2_present_mu);
    if (f >= 0) close(f);

    if (prof) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        static double acc = 0.0; static int n = 0; static struct timespec tfirst;
        if (n == 0) tfirst = t0;
        acc += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (++n >= 30) {
            double wall = (t1.tv_sec - tfirst.tv_sec) * 1e3 + (t1.tv_nsec - tfirst.tv_nsec) / 1e6;
            fprintf(stderr, "drmadapter: present_cpu avg %.2f ms | %.1f fps (%dx%d)\n",
                    acc / n, n * 1000.0 / wall, frame_w, frame_h);
            acc = 0.0; n = 0;
        }
    }
    return 0;
}

/* Worker-thread plumbing: one pending slot, latest-wins is not allowed --
 * every queued frame is presented (the enqueuer waits for a free slot, at
 * most one present ~5ms) so frames are never silently dropped. */
static pthread_mutex_t pc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pc_cv = PTHREAD_COND_INITIALIZER;
static const void *pc_src = NULL;
static uint32_t    pc_pitch = 0;
static int pc_pending = 0, pc_busy = 0, pc_thread_up = 0;

static void *present_cpu_worker(void *ud) {
    (void)ud;
    for (;;) {
        pthread_mutex_lock(&pc_mu);
        while (!pc_pending)
            pthread_cond_wait(&pc_cv, &pc_mu);
        const void *src = pc_src;
        uint32_t pitch = pc_pitch;
        pc_pending = 0;
        pc_busy = 1;
        pthread_mutex_unlock(&pc_mu);

        present_cpu_sync(src, pitch);

        pthread_mutex_lock(&pc_mu);
        pc_busy = 0;
        pthread_cond_broadcast(&pc_cv);
        pthread_mutex_unlock(&pc_mu);
    }
    return NULL;
}

static int drmadapter_present_cpu(const void *src, uint32_t src_pitch) {
    if (!hwc2_ready) {
        /* HWC2 comes up on a background thread and the compositor's first
         * frames can beat it. Wait briefly instead of dropping them: a dropped
         * opening frame leaves the panel black until the next damage, and the
         * caller's fallback path would touch gralloc before it's loaded. */
        for (int i = 0; i < 60 && !hwc2_ready; i++) usleep(50000);
    }
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || frame_w <= 0) return -1;
    if (!src) return -1;
    if (!blit_init()) return -1;

    /* Escape hatch: present synchronously inside the flip call. */
    static int sync_mode = -1;
    if (sync_mode < 0) sync_mode = getenv("DRMADAPTER_SYNC_PRESENT") ? 1 : 0;
    if (sync_mode)
        return present_cpu_sync(src, src_pitch);

    pthread_mutex_lock(&pc_mu);
    if (!pc_thread_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, present_cpu_worker, NULL) != 0) {
            pthread_mutex_unlock(&pc_mu);
            return present_cpu_sync(src, src_pitch);
        }
        pthread_detach(t);
        pc_thread_up = 1;
    }
    /* Wait for the slot (bounded): the worker takes ~5ms per frame, always
     * shorter than the compositor's own frame production. */
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 100000000L;                       /* +100ms */
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    while (pc_pending || pc_busy) {
        if (pthread_cond_timedwait(&pc_cv, &pc_mu, &dl) == ETIMEDOUT)
            break;
    }
    pc_src = src;
    pc_pitch = src_pitch;
    pc_pending = 1;
    pthread_cond_broadcast(&pc_cv);
    pthread_mutex_unlock(&pc_mu);
    return 0;
}

static EGLNativeWindowType drmadapterws_CreateWindow(EGLNativeWindowType win,
                                                      struct _EGLDisplay *display) {
    LOG("CreateWindow win=%p", (void*)win);
    /* Wait for HWC2 init to complete (may be running in background thread) */
    int retries = 0;
    while (!hwc2_ready && retries < 100) {
        usleep(50000); /* 50ms */
        retries++;
    }
    if (!hwc2_ready) {
        LOG("CreateWindow: HWC2 not ready after 5s, trying direct init");
        init_hwc2();
    }
    /* Create a FRESH native window for every surface. A single shared window
     * is a use-after-free: eglDestroyWindowSurface() destroys the underlying
     * HWCNativeWindow, so reusing one cached pointer hands mutter a dangling
     * window after the first surface is torn down (e.g. on a scale change),
     * and presents silently stop -> black screen. One window per surface lets
     * eglDestroySurface reclaim each independently. */
    struct ANativeWindow *w = NULL;
    if (hwc2_disp && frame_w > 0 && frame_h > 0)
        w = HWCNativeWindowCreate(frame_w, frame_h, HAL_PIXEL_FORMAT_RGBA_8888,
                                  present_cb, NULL);
    dtracef("CreateWindow win=%p -> fresh native=%p\n", (void*)win, (void*)w);
    return (EGLNativeWindowType)w;
}

static void drmadapterws_DestroyWindow(EGLNativeWindowType win) {
    /* eglDestroyWindowSurface() already destroys the HWCNativeWindow (see
     * hwcomposer.h), so nothing to free here -- doing so would double-free. */
    LOG("DestroyWindow win=%p", (void*)win);
    dtracef("DestroyWindow win=%p\n", (void*)win);
}

/* dmabuf import advertisement for KMS-native compositors (KWin).
 *
 * KWin builds its buffer-format table exclusively from
 * EGL_EXT_image_dma_buf_import(_modifiers) + eglQueryDmaBufFormats/ModifiersEXT;
 * without them the format intersection is empty and output configuration fails
 * silently before any buffer is even allocated. The hybris stack genuinely
 * supports these imports -- eglplatformcommon's HYBRIS_WLROOTS bridge converts
 * EGL_LINUX_DMA_BUF_EXT imports of gbm_hybris buffers into Android native
 * buffer imports (that is how wlroots renders) -- so advertise the extension
 * with the formats the bridge maps, LINEAR only (gbm_hybris allocates linear
 * when GBM_HYBRIS_LINEAR is set, and reports modifier 0). */
#ifndef DRM_FORMAT_MOD_LINEAR_LOCAL
#define DRM_FORMAT_MOD_LINEAR_LOCAL 0ull
#endif
static const EGLint dmabuf_formats[] = {
    0x34325258, /* XR24 / DRM_FORMAT_XRGB8888 */
    0x34325241, /* AR24 / DRM_FORMAT_ARGB8888 */
    0x34324258, /* XB24 / DRM_FORMAT_XBGR8888 */
    0x34324241, /* AB24 / DRM_FORMAT_ABGR8888 */
};
#define DMABUF_NFORMATS (EGLint)(sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]))

/* Opt-in via DRMADAPTER_DMABUF. Advertising dmabuf import + LINEAR modifiers is
 * needed by KMS-native compositors (KWin) to build their buffer-format table.
 * But wlroots (phoc) reacts to the modifier list by switching its output
 * allocation to gbm_bo_create_with_modifiers(), which gbm_hybris does not
 * implement -> the DRM output fails to come up and phosh never paints. So only
 * the KWin session sets this; phosh/GNOME keep the pre-existing (no-dmabuf)
 * behaviour untouched. */
static int dmabuf_ads_enabled(void) {
    static int e = -1;
    if (e < 0) e = getenv("DRMADAPTER_DMABUF") ? 1 : 0;
    return e;
}

static EGLBoolean drmadapter_eglQueryDmaBufFormatsEXT(EGLDisplay dpy, EGLint max_formats,
                                                      EGLint *formats, EGLint *num_formats) {
    (void)dpy;
    if (getenv("DRMADAPTER_TRACE_EGL"))
        fprintf(stderr, "drmadapter: eglQueryDmaBufFormatsEXT(max=%d)\n", max_formats);
    if (!num_formats) return EGL_FALSE;
    if (max_formats <= 0 || !formats) { *num_formats = DMABUF_NFORMATS; return EGL_TRUE; }
    EGLint n = max_formats < DMABUF_NFORMATS ? max_formats : DMABUF_NFORMATS;
    for (EGLint i = 0; i < n; i++) formats[i] = dmabuf_formats[i];
    *num_formats = n;
    return EGL_TRUE;
}

static EGLBoolean drmadapter_eglQueryDmaBufModifiersEXT(EGLDisplay dpy, EGLint format,
                                                        EGLint max_modifiers, EGLuint64KHR *modifiers,
                                                        EGLBoolean *external_only, EGLint *num_modifiers) {
    (void)dpy;
    if (getenv("DRMADAPTER_TRACE_EGL"))
        fprintf(stderr, "drmadapter: eglQueryDmaBufModifiersEXT(fmt=0x%x max=%d)\n", format, max_modifiers);
    int known = 0;
    for (EGLint i = 0; i < DMABUF_NFORMATS; i++)
        if (dmabuf_formats[i] == format) { known = 1; break; }
    if (!known || !num_modifiers) return EGL_FALSE;
    if (max_modifiers <= 0 || !modifiers) { *num_modifiers = 1; return EGL_TRUE; }
    modifiers[0] = DRM_FORMAT_MOD_LINEAR_LOCAL;
    if (external_only) external_only[0] = EGL_FALSE;
    *num_modifiers = 1;
    return EGL_TRUE;
}

/* Alias the ES2 extension names of a handful of buffer-mapping entry points to
 * their ES3 core implementations (which the Mali libGLESv2 exports). KWin 6's
 * GL renderer uses glMapBufferRange; on an ES2 context libepoxy will only call
 * it if it can resolve the GL_EXT_map_buffer_range / GL_OES_mapbuffer suffixed
 * form. Providing these lets KWin run on the ES2 context that phosh/GNOME use,
 * instead of an ES3 context -- the Mali ES3 driver path corrupts memory under
 * KWin's multithreaded rendering (wild jumps into the QtQml heap). Opt-in via
 * DRMADAPTER_GL_ALIASES so only the KWin session sees the aliases. */
static int gl_aliases_enabled(void) {
    static int e = -1;
    if (e < 0) e = getenv("DRMADAPTER_GL_ALIASES") ? 1 : 0;
    return e;
}

/* Diagnostic wrapper: log what libepoxy's version detection sees. If glGetString
 * returns NULL/garbage here, the context isn't current when KWin renders. */
static const GLubyte *(*real_glGetString)(GLenum) = NULL;
static const GLubyte *drmadapter_glGetString(GLenum name) {
    if (!real_glGetString)
        real_glGetString = (const GLubyte *(*)(GLenum))
            eglplatformcommon_eglGetProcAddress("glGetString");
    const GLubyte *r = real_glGetString ? real_glGetString(name) : NULL;
    if (getenv("DRMADAPTER_GL_DEBUG"))
        fprintf(stderr, "drmadapter: glGetString(0x%x)=[%s] ctx=%p draw=%p err=0x%x\n",
                name, r ? (const char *)r : "(NULL)",
                (void *)eglGetCurrentContext(),
                (void *)eglGetCurrentSurface(EGL_DRAW), eglGetError());
    return r;
}

/* Serialise shader compile/link with a glFinish. Mali (Valhall) compiles/links
 * shaders asynchronously on an internal worker thread; KWin keeps issuing GL on
 * the main thread while that worker runs, which corrupts driver/heap state
 * (shader link fails with an empty infolog, epoxy's version cache gets
 * scribbled) -> crash drawing the first client texture. Forcing the worker to
 * complete before the caller continues avoids the race. glvnd resolves the
 * vendor's GL entry points through this eglGetProcAddress, so wrapping here
 * reaches KWin's calls. Opt-in via DRMADAPTER_GL_SYNC_COMPILE. */
static int gl_sync_compile_enabled(void) {
    static int e = -1;
    if (e < 0) e = getenv("DRMADAPTER_GL_SYNC_COMPILE") ? 1 : 0;
    return e;
}
static void (*real_glLinkProgram)(GLuint) = NULL;
static void (*real_glCompileShader)(GLuint) = NULL;
static void (*real_glFinish_sc)(void) = NULL;
static void drmadapter_glLinkProgram(GLuint p) {
    if (!real_glLinkProgram) real_glLinkProgram = (void(*)(GLuint))eglplatformcommon_eglGetProcAddress("glLinkProgram");
    if (!real_glFinish_sc)   real_glFinish_sc   = (void(*)(void))eglplatformcommon_eglGetProcAddress("glFinish");
    if (real_glLinkProgram) real_glLinkProgram(p);
    if (real_glFinish_sc) real_glFinish_sc();
}
static void drmadapter_glCompileShader(GLuint s) {
    if (!real_glCompileShader) real_glCompileShader = (void(*)(GLuint))eglplatformcommon_eglGetProcAddress("glCompileShader");
    if (!real_glFinish_sc)     real_glFinish_sc     = (void(*)(void))eglplatformcommon_eglGetProcAddress("glFinish");
    if (real_glCompileShader) real_glCompileShader(s);
    if (real_glFinish_sc) real_glFinish_sc();
}

static __eglMustCastToProperFunctionPointerType
drmadapterws_eglGetProcAddress(const char *procname) {
    if (getenv("DRMADAPTER_GL_DEBUG") && strcmp(procname, "glGetString") == 0)
        return (__eglMustCastToProperFunctionPointerType)drmadapter_glGetString;
    if (gl_sync_compile_enabled()) {
        if (strcmp(procname, "glLinkProgram") == 0)
            return (__eglMustCastToProperFunctionPointerType)drmadapter_glLinkProgram;
        if (strcmp(procname, "glCompileShader") == 0)
            return (__eglMustCastToProperFunctionPointerType)drmadapter_glCompileShader;
    }
    if (dmabuf_ads_enabled()) {
        if (strcmp(procname, "eglQueryDmaBufFormatsEXT") == 0)
            return (__eglMustCastToProperFunctionPointerType)drmadapter_eglQueryDmaBufFormatsEXT;
        if (strcmp(procname, "eglQueryDmaBufModifiersEXT") == 0)
            return (__eglMustCastToProperFunctionPointerType)drmadapter_eglQueryDmaBufModifiersEXT;
    }
    if (gl_aliases_enabled()) {
        const char *core = NULL;
        if      (strcmp(procname, "glMapBufferRangeEXT") == 0)         core = "glMapBufferRange";
        else if (strcmp(procname, "glFlushMappedBufferRangeEXT") == 0) core = "glFlushMappedBufferRange";
        else if (strcmp(procname, "glUnmapBufferOES") == 0)            core = "glUnmapBuffer";
        else if (strcmp(procname, "glMapBufferOES") == 0)              core = "glMapBuffer";
        else if (strcmp(procname, "glGetBufferPointervOES") == 0)      core = "glGetBufferPointerv";
        if (core)
            return eglplatformcommon_eglGetProcAddress(core);
    }
    return eglplatformcommon_eglGetProcAddress(procname);
}

static void drmadapterws_passthroughImageKHR(EGLContext *ctx, EGLenum *target,
                                              EGLClientBuffer *buffer,
                                              const EGLint **attrib_list) {
    eglplatformcommon_passthroughImageKHR(ctx, target, buffer, attrib_list);
}

static const char *drmadapterws_eglQueryString(EGLDisplay dpy, EGLint name,
    const char *(*real_eglQueryString)(EGLDisplay, EGLint)) {
    const char *ret = eglplatformcommon_eglQueryString(dpy, name, real_eglQueryString);
    if (getenv("DRMADAPTER_TRACE_EGL") && name == EGL_EXTENSIONS)
        fprintf(stderr, "drmadapter: eglQueryString(EXTENSIONS) dpy=%p ret=%p\n", (void*)dpy, (void*)ret);
    if (name == EGL_EXTENSIONS && ret && dmabuf_ads_enabled()) {
        /* Advertise the dmabuf import path the hybris bridge implements. */
        static char extbuf[4096];
        if (!strstr(ret, "EGL_EXT_image_dma_buf_import_modifiers")) {
            const char *base = strstr(ret, "EGL_EXT_image_dma_buf_import") ? "" : " EGL_EXT_image_dma_buf_import";
            snprintf(extbuf, sizeof extbuf,
                     "%s%s EGL_EXT_image_dma_buf_import_modifiers", ret, base);
            if (getenv("DRMADAPTER_TRACE_EGL")) {
                size_t l = strlen(extbuf);
                fprintf(stderr, "drmadapter: extensions APPENDED, tail: ...%s\n",
                        extbuf + (l > 100 ? l - 100 : 0));
            }
            return extbuf;
        }
        if (getenv("DRMADAPTER_TRACE_EGL"))
            fprintf(stderr, "drmadapter: extensions already contain dma_buf_import\n");
    }
    return ret;
}

static void drmadapterws_prepareSwap(EGLDisplay dpy, EGLNativeWindowType win,
                                      EGLint *damage_rects, EGLint damage_n_rects) {}

static void drmadapterws_finishSwap(EGLDisplay dpy, EGLNativeWindowType win) {}

static void drmadapterws_setSwapInterval(EGLDisplay dpy, EGLNativeWindowType win,
                                          EGLint interval) {}

struct ws_module ws_module_info = {
    .init_module         = drmadapterws_init_module,
    .GetDisplay          = drmadapterws_GetDisplay,
    .Terminate           = drmadapterws_Terminate,
    .CreateWindow        = drmadapterws_CreateWindow,
    .DestroyWindow       = drmadapterws_DestroyWindow,
    .eglGetProcAddress   = drmadapterws_eglGetProcAddress,
    .passthroughImageKHR = drmadapterws_passthroughImageKHR,
    .eglQueryString      = drmadapterws_eglQueryString,
    .prepareSwap         = drmadapterws_prepareSwap,
    .finishSwap          = drmadapterws_finishSwap,
    .setSwapInterval     = drmadapterws_setSwapInterval,
    .releaseDisplay      = drmadapterws_releaseDisplay,
    .eglInitialized      = drmadapterws_eglInitialized,
};
