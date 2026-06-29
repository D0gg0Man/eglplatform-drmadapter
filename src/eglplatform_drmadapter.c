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
#include <unistd.h>
#include <pthread.h>
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
static void on_vsync(HWC2EventListener *s,int32_t q,hwc2_display_t d,int64_t t){}
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
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || !buf) {
        dtracef("present_cb #%lu SKIP ready=%d disp=%p layer=%p buf=%p\n",
                pn, hwc2_ready, (void*)hwc2_disp, (void*)hwc2_layer, (void*)buf);
        return;
    }
    uint32_t nt = 0, nr = 0;
    int vr = hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (nt) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, 0, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, buf, -1, 0);
    int32_t fence = -1;
    int pr = hwc2_compat_display_present(hwc2_disp, &fence);
    dtracef("present_cb #%lu validate=%d nt=%u nr=%u present=%d fence=%d buf=%p\n",
            pn, vr, nt, nr, pr, fence, (void*)buf);
    HWCNativeBufferSetFence(buf, -1);
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

/* HWC2 only scans out buffers that come from its own HWCNativeWindow queue (the
 * path mutter's eglSwapBuffers drives via present_cb). Presenting wlroots' raw
 * gbm_hybris buffer through set_client_target returns success but never reaches
 * the panel. So instead we run a tiny GLES2 blitter: import wlroots' rendered
 * buffer as a texture and draw it into an HWCNativeWindow surface, then
 * eglSwapBuffers -> present_cb -> HWC2. The window's buffer is HWC2-scanout
 * compatible, so this actually lights up the display. */
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
static EGLDisplay   bl_dpy  = EGL_NO_DISPLAY;
static EGLContext   bl_ctx  = EGL_NO_CONTEXT;
static EGLSurface   bl_surf = EGL_NO_SURFACE;
static struct ANativeWindow *bl_win = NULL;
static GLuint       bl_prog = 0, bl_tex = 0, bl_pos = 0;
static int          bl_ready = 0, bl_failed = 0;
static PFNEGLCREATEIMAGEKHRPROC  p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

static GLuint bl_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOG("blit shader err: %s", log); }
    return s;
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
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(bl_dpy, cfgattr, &cfg, 1, &n) || n < 1) { LOG("blit: no EGL config"); bl_failed = 1; return 0; }

    bl_win = HWCNativeWindowCreate(frame_w, frame_h, HAL_PIXEL_FORMAT_RGBA_8888, present_cb, NULL);
    if (!bl_win) { LOG("blit: HWCNativeWindowCreate failed"); bl_failed = 1; return 0; }
    bl_surf = eglCreateWindowSurface(bl_dpy, cfg, (EGLNativeWindowType)bl_win, NULL);
    if (bl_surf == EGL_NO_SURFACE) { LOG("blit: eglCreateWindowSurface failed 0x%x", eglGetError()); bl_failed = 1; return 0; }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    bl_ctx = eglCreateContext(bl_dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (bl_ctx == EGL_NO_CONTEXT) { LOG("blit: eglCreateContext failed 0x%x", eglGetError()); bl_failed = 1; return 0; }

    p_eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) { LOG("blit: missing EGL image procs"); bl_failed = 1; return 0; }

    EGLContext sc = eglGetCurrentContext();
    EGLSurface sd = eglGetCurrentSurface(EGL_DRAW), sr = eglGetCurrentSurface(EGL_READ);
    eglMakeCurrent(bl_dpy, bl_surf, bl_surf, bl_ctx);

    static const char *vs =
        "attribute vec2 pos;\nvarying vec2 uv;\n"
        "void main(){ uv = vec2((pos.x+1.0)*0.5, (1.0-pos.y)*0.5); gl_Position = vec4(pos,0.0,1.0); }\n";
    static const char *fs =
        "precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\n"
        "void main(){ gl_FragColor = texture2D(tex, uv); }\n";
    bl_prog = glCreateProgram();
    glAttachShader(bl_prog, bl_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(bl_prog, bl_shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(bl_prog, 0, "pos");
    glLinkProgram(bl_prog);
    bl_pos = 0;
    glGenTextures(1, &bl_tex);
    glBindTexture(GL_TEXTURE_2D, bl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Restore whatever was current (often nothing -- wlroots unbinds after
     * rendering). Use bl_dpy, not eglGetCurrentDisplay(), which is nil here. */
    eglMakeCurrent(bl_dpy, sd, sr, sc);
    bl_ready = 1;
    LOG("blit: initialised (%dx%d)", frame_w, frame_h);
    return 1;
}

/* Per-handle cache of the imported EGLImage + GL texture for the blit source.
 * The EGLImage references the gralloc buffer's live memory, so one import per
 * buffer suffices -- sampling it each frame shows whatever wlroots last drew.
 * A fresh RemoteWindowBuffer (not wlroots' own) is imported because Mali rejects
 * a second eglCreateImageKHR on an already-imported ANativeWindowBuffer. Must be
 * called with the blit context current. */
#define BLIT_CACHE_MAX 8
static struct { buffer_handle_t handle; EGLImageKHR img; GLuint tex; } bl_cache[BLIT_CACHE_MAX];
static int bl_cache_n = 0;
static GLuint blit_tex_for_handle(buffer_handle_t handle) {
    for (int i = 0; i < bl_cache_n; i++)
        if (bl_cache[i].handle == handle) {
            /* Re-associate the EGLImage with the texture every frame: the image
             * references the gralloc buffer's live memory, but the GL texture
             * cache can hold a stale snapshot, so without re-binding the blit
             * shows an old frame -> flicker. This is cheap (no re-import). */
            glBindTexture(GL_TEXTURE_2D, bl_cache[i].tex);
            p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)bl_cache[i].img);
            return bl_cache[i].tex;
        }
    if (bl_cache_n >= BLIT_CACHE_MAX) return 0;
    struct ANativeWindowBuffer *anwb =
        (struct ANativeWindowBuffer *)eglplatformcommon_wlr_make_anwb(handle);
    if (!anwb) return 0;
    EGLImageKHR img = p_eglCreateImageKHR(bl_dpy, EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)anwb, NULL);
    if (img == EGL_NO_IMAGE_KHR) {
        LOG("blit: eglCreateImageKHR(fresh anwb) failed 0x%x", eglGetError());
        return 0;
    }
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
    LOG("blit: cached image+texture for handle %p (tex %u)", (void*)handle, tex);
    return tex;
}

static int drmadapter_present_gralloc(buffer_handle_t handle) {
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || frame_w <= 0) return -1;
    struct ANativeWindowBuffer *src =
        (struct ANativeWindowBuffer *)eglplatformcommon_wlr_lookup_anwb(handle);
    if (!src) return -1;
    if (!blit_init()) return -1;

    /* Save wlroots' current EGL state (frequently nothing -- it unbinds after
     * rendering), switch to the blitter, draw, swap. Restore via bl_dpy. */
    EGLContext sc = eglGetCurrentContext();
    EGLSurface sd = eglGetCurrentSurface(EGL_DRAW), sr = eglGetCurrentSurface(EGL_READ);

    /* GPU sync is handled upstream now: libdrm-hybris reports DRM_CAP_SYNCOBJ_
     * TIMELINE unsupported so wlroots uses implicit fencing on the buffer, which
     * Mali honours when the blit samples it. (Making wlroots' own context
     * current here to glFinish corrupted its rendering -> black, so we don't.) */
    if (!eglMakeCurrent(bl_dpy, bl_surf, bl_surf, bl_ctx)) {
        static int w = 0; if (!w) { LOG("blit: makecurrent failed 0x%x", eglGetError()); w = 1; }
        return -1;
    }

    GLuint tex = blit_tex_for_handle(handle);
    if (tex) {
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
        eglSwapBuffers(bl_dpy, bl_surf); /* -> present_cb -> HWC2 */
    }

    eglMakeCurrent(bl_dpy, sd, sr, sc); /* restore wlroots state (sd/sc may be none) */
    static unsigned long pn = 0;
    if ((pn++ % 120) == 0)
        dtracef("present_gralloc(blit) #%lu handle=%p tex=%u\n", pn, (void*)handle, tex);
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

static __eglMustCastToProperFunctionPointerType
drmadapterws_eglGetProcAddress(const char *procname) {
    return eglplatformcommon_eglGetProcAddress(procname);
}

static void drmadapterws_passthroughImageKHR(EGLContext *ctx, EGLenum *target,
                                              EGLClientBuffer *buffer,
                                              const EGLint **attrib_list) {
    eglplatformcommon_passthroughImageKHR(ctx, target, buffer, attrib_list);
}

static const char *drmadapterws_eglQueryString(EGLDisplay dpy, EGLint name,
    const char *(*real_eglQueryString)(EGLDisplay, EGLint)) {
    return eglplatformcommon_eglQueryString(dpy, name, real_eglQueryString);
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
