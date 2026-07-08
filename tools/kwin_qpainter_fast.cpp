/* kwin_qpainter_fast.so -- LD_PRELOAD fast path for KWin's software renderer.
 *
 * KWin's QPainter scene draws every client surface with
 * QPainter::drawImage(QRectF, QImage, QRectF). At any output scale != 1 the
 * painter carries a scale transform, which pushes Qt's raster engine into its
 * per-pixel transformed-sampling path (~6-8 ms per fullscreen 1080p frame on
 * this SoC) even though the mapping is pixel-exact 1:1 (buffer_scale ==
 * output scale). This shim interposes that one exported symbol and, when the
 * draw really is an untransformed 1:1 blit, does it directly:
 *   - opaque sources (RGB32) or CompositionMode_Source: row memcpy
 *   - premultiplied ARGB32 SourceOver: NEON blend (~2-3 ms/Mpx)
 * Anything else (fractional mapping, rotations, opacity, exotic formats)
 * falls through to the real Qt implementation.
 *
 * Env-gated by KWIN_QPAINTER_FAST=1 -- only the KWin session sets it; every
 * other process calls straight through to Qt. */

#include <QPainter>
#include <QImage>
#include <QTransform>
#include <QRegion>
#include <QPaintDevice>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

typedef void (*real_drawimage_fn)(QPainter *, const QRectF &, const QImage &,
                                  const QRectF &, Qt::ImageConversionFlags);

static real_drawimage_fn real_drawImage;

static bool fast_enabled()
{
    static int e = -1;
    if (e < 0)
        e = getenv("KWIN_QPAINTER_FAST") ? 1 : 0;
    return e == 1;
}

/* KWIN_QPAINTER_FAST_DEBUG=1: periodic call/hit/reject telemetry. */
static struct {
    unsigned long calls, hits, dev, fmt, op, mode, xform, align, bounds;
    unsigned long fast_ns, blit_ns, mpx, real_px;
} st;
static void stat_dump()
{
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("KWIN_QPAINTER_FAST_DEBUG") ? 1 : 0;
    if (dbg && (st.calls % 300) == 0)
        fprintf(stderr, "qpainter_fast: calls=%lu hits=%lu fastms=%.1f blitms=%.1f srcmpx=%.1f blitmpx=%.1f rej dev=%lu fmt=%lu op=%lu mode=%lu xform=%lu align=%lu bounds=%lu\n",
                st.calls, st.hits, st.fast_ns / 1e6, st.blit_ns / 1e6, st.mpx / 1e6, st.real_px / 1e6,
                st.dev, st.fmt, st.op, st.mode, st.xform, st.align, st.bounds);
}

/* dst = src OVER dst, both premultiplied ARGB32 (bytes B,G,R,A). */
static inline void row_over(uint32_t *d, const uint32_t *s, int n)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
    for (; x + 8 <= n; x += 8) {
        uint8x8x4_t sp = vld4_u8((const uint8_t *)(s + x));
        uint8x8x4_t dp = vld4_u8((const uint8_t *)(d + x));
        uint8x8_t inva = vmvn_u8(sp.val[3]);
        for (int c = 0; c < 4; c++) {
            uint16x8_t t = vmull_u8(dp.val[c], inva);
            t = vsraq_n_u16(t, t, 8);                 /* t += t >> 8 */
            dp.val[c] = vadd_u8(sp.val[c], vrshrn_n_u16(t, 8));
        }
        vst4_u8((uint8_t *)(d + x), dp);
    }
#endif
    for (; x < n; x++) {
        uint32_t sv = s[x];
        uint32_t a = sv >> 24;
        if (a == 0xFF) {
            d[x] = sv;
            continue;
        }
        if (a == 0) {
            continue;
        }
        uint32_t dv = d[x], inva = 255 - a;
        uint32_t rb = (dv & 0xFF00FFu) * inva + 0x800080u;
        rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
        uint32_t ag = ((dv >> 8) & 0xFF00FFu) * inva + 0x800080u;
        ag = (ag + ((ag >> 8) & 0xFF00FFu)) & 0xFF00FF00u;
        d[x] = sv + (rb | ag);
    }
}

static bool try_fast_blit(QPainter *p, const QRectF &targetRect,
                          const QImage &image, const QRectF &sourceRect)
{
    /* Raster paint device backed by a QImage. */
    QPaintDevice *pd = p->device();
    if (!pd || pd->devType() != QInternal::Image) {
        st.dev++;
        return false;
    }
    QImage *dst = static_cast<QImage *>(pd);

    const QImage::Format df = dst->format();
    const QImage::Format sf = image.format();
    if (df != QImage::Format_RGB32 && df != QImage::Format_ARGB32_Premultiplied) {
        st.fmt++;
        return false;
    }
    if (sf != QImage::Format_RGB32 && sf != QImage::Format_ARGB32_Premultiplied) {
        st.fmt++;
        return false;
    }

    if (p->opacity() < 1.0) {
        st.op++;
        return false;
    }
    const QPainter::CompositionMode mode = p->compositionMode();
    if (mode != QPainter::CompositionMode_SourceOver && mode != QPainter::CompositionMode_Source) {
        st.mode++;
        return false;
    }

    const QTransform t = p->combinedTransform();
    if (t.type() > QTransform::TxScale) {
        st.xform++;
        return false;
    }

    /* The mapping must be pixel-exact 1:1: device target size == source size,
     * both integer-aligned. */
    const QRectF devTf = t.mapRect(targetRect);
    const qreal eps = 1.0 / 256.0;
    if (qAbs(devTf.width() - sourceRect.width()) > eps || qAbs(devTf.height() - sourceRect.height()) > eps) {
        st.align++;
        return false;
    }
    if (qAbs(devTf.x() - qRound(devTf.x())) > eps || qAbs(devTf.y() - qRound(devTf.y())) > eps) {
        st.align++;
        return false;
    }
    if (qAbs(sourceRect.x() - qRound(sourceRect.x())) > eps || qAbs(sourceRect.y() - qRound(sourceRect.y())) > eps) {
        st.align++;
        return false;
    }

    QRect devT(qRound(devTf.x()), qRound(devTf.y()), qRound(devTf.width()), qRound(devTf.height()));
    const QPoint srcOrigin(qRound(sourceRect.x()), qRound(sourceRect.y()));

    /* Clip: painter clip (logical -> device) intersected with target + bounds. */
    QRegion devClip(dst->rect());
    if (p->hasClipping())
        devClip &= t.map(p->clipRegion());
    devClip &= devT;
    devClip &= QRect(QPoint(0, 0), dst->size());
    if (devClip.isEmpty())
        return true;    /* fully clipped: nothing to draw, but handled */

    /* Source bounds check. */
    const QRect needed = devClip.boundingRect().translated(srcOrigin - devT.topLeft());
    if (!image.rect().contains(needed)) {
        st.bounds++;
        return false;
    }

    const bool blend = (mode == QPainter::CompositionMode_SourceOver) && image.hasAlphaChannel();
    const uchar *sbits = image.constBits();
    uchar *dbits = dst->bits();
    const qsizetype spitch = image.bytesPerLine();
    const qsizetype dpitch = dst->bytesPerLine();

    struct timespec ba, bb;
    clock_gettime(CLOCK_MONOTONIC, &ba);
    for (const QRect &r : devClip) {
        const int sx = r.x() - devT.x() + srcOrigin.x();
        const int sy = r.y() - devT.y() + srcOrigin.y();
        st.real_px += (unsigned long)r.width() * (unsigned long)r.height();
        for (int y = 0; y < r.height(); y++) {
            const uchar *srow = sbits + (qsizetype)(sy + y) * spitch + (qsizetype)sx * 4;
            uchar *drow = dbits + (qsizetype)(r.y() + y) * dpitch + (qsizetype)r.x() * 4;
            if (blend)
                row_over((uint32_t *)drow, (const uint32_t *)srow, r.width());
            else
                memcpy(drow, srow, (size_t)r.width() * 4);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &bb);
    st.blit_ns += (unsigned long)((bb.tv_sec - ba.tv_sec) * 1000000000L + (bb.tv_nsec - ba.tv_nsec));
    return true;
}

/* Interpose the exact exported overload KWin's renderSurfaceItem uses (the
 * 3-arg inline overload compiles into a call to this 4-arg symbol). */
void QPainter::drawImage(const QRectF &targetRect, const QImage &image,
                         const QRectF &sourceRect, Qt::ImageConversionFlags flags)
{
    if (!real_drawImage) {
        real_drawImage = (real_drawimage_fn)dlsym(
            RTLD_NEXT, "_ZN8QPainter9drawImageERK6QRectFRK6QImageS2_6QFlagsIN2Qt19ImageConversionFlagEE");
        if (!real_drawImage)
            abort();    /* interposer without a fallback would render nothing */
    }
    st.calls++;
    stat_dump();
    if (fast_enabled()) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        const bool ok = try_fast_blit(this, targetRect, image, sourceRect);
        clock_gettime(CLOCK_MONOTONIC, &b);
        st.fast_ns += (unsigned long)((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec));
        if (ok) {
            st.hits++;
            st.mpx += (unsigned long)(sourceRect.width() * sourceRect.height());
            return;
        }
    }
    real_drawImage(this, targetRect, image, sourceRect, flags);
}
