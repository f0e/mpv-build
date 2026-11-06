// Exercise the production HDR10 metadata forwarding of the Android EGL
// context: what reaches eglSurfaceAttrib for real, guessed and changing video
// metadata. No EGL or Android runtime is needed; the attribute calls are
// recorded.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef void *EGLDisplay;
typedef void *EGLSurface;

// The extension tokens as the EGL_EXT_surface_SMPTE2086_metadata and
// EGL_EXT_surface_CTA861_3_metadata specifications assign them.
#define EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT 0x3341
#define EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT 0x3342
#define EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT 0x3343
#define EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT 0x3344
#define EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT 0x3345
#define EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT 0x3346
#define EGL_SMPTE2086_WHITE_POINT_X_EXT 0x3347
#define EGL_SMPTE2086_WHITE_POINT_Y_EXT 0x3348
#define EGL_SMPTE2086_MAX_LUMINANCE_EXT 0x3349
#define EGL_SMPTE2086_MIN_LUMINANCE_EXT 0x334A
#define EGL_METADATA_SCALING_EXT 50000
#define EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT 0x3360
#define EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT 0x3361

#define MP_ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
// Consumes its arguments like mpv's logging does, so nothing reads as unused.
#define MP_VERBOSE(ctx, ...) ((void)(ctx), (void)snprintf(NULL, 0, __VA_ARGS__))

// The libplacebo and mpv types the forwarding reads, reduced to its fields.
struct pl_cie_xy { float x, y; };
struct pl_raw_primaries { struct pl_cie_xy red, green, blue, white; };
struct pl_hdr_metadata {
    struct pl_raw_primaries prim;
    float min_luma, max_luma;
    float max_cll, max_fall;
    float scene_max[3], scene_avg;
};
enum pl_color_primaries { PL_COLOR_PRIM_BT_2020 = 1 };

static bool pl_primaries_valid(const struct pl_raw_primaries *prim)
{
    return prim->red.x > 0 && prim->green.x > 0 && prim->blue.x > 0 && prim->white.x > 0;
}

static const struct pl_raw_primaries *pl_raw_primaries_get(enum pl_color_primaries prim)
{
    static const struct pl_raw_primaries bt2020 = {
        {0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, {0.3127f, 0.3290f},
    };
    return prim == PL_COLOR_PRIM_BT_2020 ? &bt2020 : NULL;
}

struct mp_image_params { struct { struct pl_hdr_metadata hdr; } color; };
struct vo { int params_mutex; struct mp_image_params *params; };
struct ra_ctx { void *priv; struct vo *vo; };
#define mp_mutex_lock(m) ((void)(m))
#define mp_mutex_unlock(m) ((void)(m))

struct priv {
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    bool pq_surface;
    bool has_smpte2086;
    bool has_cta861_3;
    bool hdr_sent;
    struct pl_hdr_metadata hdr;
};

static struct { EGLint attr, value; } calls[64];
static int ncalls;

static EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint value)
{
    (void)d; (void)s;
    if (ncalls < (int)MP_ARRAY_SIZE(calls))
        calls[ncalls++] = (__typeof__(calls[0])){attr, value};
    return 1;
}

#include "android_hdr_surface_metadata.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static int sent(EGLint attr, EGLint *value)
{
    for (int i = 0; i < ncalls; i++) {
        if (calls[i].attr == attr) {
            *value = calls[i].value;
            return 1;
        }
    }
    return 0;
}

static void expect(const char *what, EGLint attr, double nits_or_xy)
{
    EGLint value;
    EGLint want = (EGLint)lrint(nits_or_xy * EGL_METADATA_SCALING_EXT);
    CHECK(sent(attr, &value) && value == want, "%s: sent %d, want %d", what, sent(attr, &value) ? value : -1, want);
}

static struct pl_hdr_metadata streaming_1000(void)
{
    return (struct pl_hdr_metadata){
        .prim = *pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020),
        .min_luma = 0.005f, .max_luma = 1000.0f, .max_cll = 809.0f, .max_fall = 178.0f,
    };
}

static void swap(struct ra_ctx *ctx)
{
    ncalls = 0;
    update_hdr_metadata(ctx);
}

int main(void)
{
    struct mp_image_params params = {.color.hdr = streaming_1000()};
    struct vo vo = {.params = &params};
    struct priv p = {.pq_surface = true, .has_smpte2086 = true, .has_cta861_3 = true};
    struct ra_ctx ctx = {.priv = &p, .vo = &vo};

    // Real HDR10 metadata reaches every SMPTE 2086 and CTA-861.3 attribute.
    swap(&ctx);
    CHECK(ncalls == 12, "first swap made %d calls, want 12", ncalls);
    expect("red x", EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, 0.708);
    expect("red y", EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, 0.292);
    expect("green x", EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, 0.170);
    expect("green y", EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, 0.797);
    expect("blue x", EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, 0.131);
    expect("blue y", EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, 0.046);
    expect("white x", EGL_SMPTE2086_WHITE_POINT_X_EXT, 0.3127);
    expect("white y", EGL_SMPTE2086_WHITE_POINT_Y_EXT, 0.3290);
    expect("max luminance", EGL_SMPTE2086_MAX_LUMINANCE_EXT, 1000.0);
    expect("min luminance", EGL_SMPTE2086_MIN_LUMINANCE_EXT, 0.005);
    expect("MaxCLL", EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, 809.0);
    expect("MaxFALL", EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, 178.0);

    // Unchanged static metadata is not re-sent, even when per-scene values move.
    swap(&ctx);
    CHECK(ncalls == 0, "unchanged metadata re-sent (%d calls)", ncalls);
    params.color.hdr.scene_max[0] = 420.0f;
    params.color.hdr.scene_avg = 12.0f;
    swap(&ctx);
    CHECK(ncalls == 0, "per-scene change re-sent static metadata (%d calls)", ncalls);

    // A new file's metadata is sent again.
    params.color.hdr.max_cll = 1200.0f;
    swap(&ctx);
    expect("updated MaxCLL", EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, 1200.0);

    // mpv's guess for a source without metadata (PQ-ceiling mastering display,
    // no content light level) must not replace SurfaceFlinger's own default.
    params.color.hdr = (struct pl_hdr_metadata){
        .prim = *pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020), .max_luma = 10000.0f,
    };
    swap(&ctx);
    CHECK(ncalls == 0, "guessed metadata was sent (%d calls)", ncalls);

    // MaxCLL without a mastering display, and missing primaries fall back to BT.2020.
    params.color.hdr = (struct pl_hdr_metadata){.max_cll = 700.0f, .max_fall = 90.0f};
    swap(&ctx);
    CHECK(ncalls == 2, "MaxCLL-only metadata made %d calls, want 2", ncalls);
    expect("MaxCLL only", EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, 700.0);
    params.color.hdr = (struct pl_hdr_metadata){.max_luma = 4000.0f, .min_luma = 0.0001f};
    swap(&ctx);
    expect("fallback red x", EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, 0.708);
    expect("4000 nit mastering", EGL_SMPTE2086_MAX_LUMINANCE_EXT, 4000.0);

    // Without a PQ surface or the extensions nothing is attempted.
    struct priv sdr = {.pq_surface = false, .has_smpte2086 = true, .has_cta861_3 = true};
    ctx.priv = &sdr;
    params.color.hdr = streaming_1000();
    swap(&ctx);
    CHECK(ncalls == 0, "sRGB surface got metadata (%d calls)", ncalls);
    struct priv noext = {.pq_surface = true};
    ctx.priv = &noext;
    swap(&ctx);
    CHECK(ncalls == 0, "surface without the extensions got metadata (%d calls)", ncalls);

    if (failures)
        return 1;
    printf("android hdr surface metadata: ok\n");
    return 0;
}
