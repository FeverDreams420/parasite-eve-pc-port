/* Precise animation: model matrices kept at full precision for HD.
 *
 * The game builds each body part's matrix from its parent's with the GTE
 * (Render_TransformVertices: rotation by rotation, the part's offset through
 * the parent's rotation plus its translation), and every step drops the
 * fraction: rotations to 1/4096, positions to whole units. Each frame it then
 * puts every part's matrix through the camera's the same way
 * (Render_TransformMorphVertices) before projecting the part's vertices. The
 * dropped fractions change as a model moves or turns, so parts deep in the
 * chain (legs, arms) shake by a few units against each other. At 1x that is
 * a fraction of a pixel and mostly hidden by the whole-pixel rounding; at
 * HD with precise geometry it shows.
 *
 * Here the same steps run alongside in floating point (host patch of
 * Render_VertexTransforms.c), from the same inputs the game uses: the
 * model's own matrix, the animation's part rotations and offsets, the
 * camera. Only the dropped fractions differ. The result is handed to PsyZ's
 * precise geometry for the part's projections (Psyz_PgxpSetFloatMatrix),
 * which only uses it while the GTE holds the matching integer matrix and the
 * position it gives is within a few pixels of the game's own. The game's own
 * integer results are not changed, so collision, shadows and everything else
 * work exactly as before, and 1x is untouched.
 *
 * A matrix in memory is 8 words: 3x3 rotation (16-bit, 4096 = 1.0, row by
 * row), 16 bits unused, then the translation as 3 words.
 */
#include <psyz.h>
#include <psyz/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pe_host_graphics_is_hd(void);

/* Settings from the environment, read once: these checks run for every part,
 * face and corner, thousands of times a frame, and getenv is slow on Windows
 * (it cost about 15 ms a frame at HD there). */
static int env_once(int *cache, const char *name) {
    if (*cache < 0) {
        *cache = getenv(name) != NULL;
    }
    return *cache;
}
static int e_no_anim = -1, e_log_pa = -1, e_log_pa2 = -1, e_no_nclip = -1, e_log_pa3 = -1;
#define NO_PRECISE_ANIM env_once(&e_no_anim, "PE_NO_PRECISE_ANIM")
#define LOG_PA env_once(&e_log_pa, "PE_LOG_PA")
#define LOG_PA2 env_once(&e_log_pa2, "PE_LOG_PA2")
#define NO_PRECISE_NCLIP env_once(&e_no_nclip, "PE_NO_PRECISE_NCLIP")
#define LOG_PA3 env_once(&e_log_pa3, "PE_LOG_PA3")

typedef struct {
    float r[9]; /* 4096 = 1.0 */
    float t[3];
} FMat;

/* Full-precision copies of the part matrices the game stored, by address,
 * with the integer matrix they belong to (a copy only counts while memory
 * still holds that matrix). */
#define SLOTS 4096
static struct {
    const void *addr;
    int words[8];
    FMat f;
} g_shadow[SLOTS];

static FMat g_cur, g_stack[32];
static int g_sp, g_on;

static unsigned slot_of(const void *addr) { return ((unsigned)(size_t)addr >> 5) & (SLOTS - 1); }

static void from_int(const int *w, FMat *f) {
    const short *m = (const short *)w;
    int i;
    for (i = 0; i < 9; i++) {
        f->r[i] = m[i];
    }
    for (i = 0; i < 3; i++) {
        f->t[i] = (float)w[5 + i];
    }
}

/* The full-precision copy of the matrix at addr, if memory still holds the
 * integer matrix it was made with. */
static const FMat *lookup(const int *addr) {
    unsigned s = slot_of(addr);
    if (g_shadow[s].addr == addr && !memcmp(g_shadow[s].words, addr, sizeof(g_shadow[s].words))) {
        return &g_shadow[s].f;
    }
    return NULL;
}

/* a x b, both 4096 = 1.0 */
static void mul_rot(const float *a, const float *b, float *out) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            out[i * 3 + j] =
                (a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j]) /
                4096.0f;
        }
    }
}

/* t + r x v */
static void apply(const float *r, const float *t, const float *v, float *out) {
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = t[i] + (r[i * 3 + 0] * v[0] + r[i * 3 + 1] * v[1] + r[i * 3 + 2] * v[2]) / 4096.0f;
    }
}

/* Render_TransformVertices: the model's own matrix was loaded. */
void pe_host_pa_begin(const int *root) {
    const FMat *f;
    g_on = pe_host_graphics_is_hd() && !NO_PRECISE_ANIM;
    if (!g_on) {
        return;
    }
    g_sp = 0;
    if (LOG_PA) {
        static int n;
        if (n++ < 4) {
            fprintf(stderr, "pb: root %p\n", (const void *)root);
        }
    }
    f = lookup(root);
    if (f) {
        g_cur = *f;
    } else {
        from_int(root, &g_cur);
    }
}

/* Its matrix stack: the current matrix saved / taken back. */
void pe_host_pa_push(void) {
    if (g_on && g_sp < (int)(sizeof(g_stack) / sizeof(g_stack[0]))) {
        g_stack[g_sp] = g_cur;
    }
    g_sp++;
}

void pe_host_pa_pop(void) {
    if (!g_on) {
        return;
    }
    g_sp--;
    if (g_sp >= 0 && g_sp < (int)(sizeof(g_stack) / sizeof(g_stack[0]))) {
        g_cur = g_stack[g_sp];
    } else {
        g_on = 0; /* lost track: the rest of this model stays integer */
    }
}

/* A part's matrix is about to be built from the animation's matrix at src.
 * The game builds it in place (src is the part's own matrix), so its input
 * is kept first. */
static int g_src[8];
void pe_host_pa_local(const int *src) {
    if (g_on) {
        memcpy(g_src, src, sizeof(g_src));
    }
}

/* The part's matrix was built at dst from the current one and the kept
 * input; the vector the game moved it by is the input's own translation
 * (root, use_src) or vec, the part's offset along Z. It becomes current. */
void pe_host_pa_child(const int *dst, int use_src, const int *vec) {
    FMat local, child;
    float v[3];
    unsigned s;
    int i;
    if (!g_on) {
        return;
    }
    if (use_src) {
        vec = g_src + 5;
    }
    from_int(g_src, &local);
    mul_rot(g_cur.r, local.r, child.r);
    for (i = 0; i < 3; i++) {
        v[i] = (short)vec[i]; /* the GTE takes the low 16 bits */
    }
    apply(g_cur.r, g_cur.t, v, child.t);
    if (LOG_PA) { /* debug: ours vs the game's part matrix */
        static int n;
        FMat d;
        from_int(dst, &d);
        if (n++ < 16) {
            fprintf(stderr, "pc: sp %d cur r %.0f %.0f %.0f %.0f t %.0f %.0f %.0f | src r %.0f %.0f %.0f %.0f v %.0f %.0f %.0f\n"
                    "pc:   dst r %.0f %.0f %.0f %.0f t %.0f %.0f %.0f | ours r %.1f %.1f %.1f %.1f t %.1f %.1f %.1f\n",
                    g_sp, g_cur.r[0], g_cur.r[1], g_cur.r[3], g_cur.r[4], g_cur.t[0], g_cur.t[1], g_cur.t[2],
                    local.r[0], local.r[1], local.r[3], local.r[4], v[0], v[1], v[2],
                    d.r[0], d.r[1], d.r[3], d.r[4], d.t[0], d.t[1], d.t[2],
                    child.r[0], child.r[1], child.r[3], child.r[4], child.t[0], child.t[1], child.t[2]);
        }
    }
    s = slot_of(dst);
    g_shadow[s].addr = dst;
    memcpy(g_shadow[s].words, dst, sizeof(g_shadow[s].words));
    g_shadow[s].f = child;
    g_cur = child;
}

/* Render_TransformMorphVertices: the part's matrix through the camera was
 * loaded into the GTE; its vertices are projected next. */
unsigned int pe_host_frame_number(void);
static void log_status(const int *part, int ok) { /* debug: a part changing path */
    static const int *addr[512];
    static signed char last[512];
    unsigned i;
    for (i = 0; i < 512 && addr[i] && addr[i] != part; i++) {
    }
    if (i == 512) {
        return;
    }
    if (!addr[i]) {
        addr[i] = part;
        last[i] = (signed char)ok;
        return;
    }
    if (last[i] != ok) {
        fprintf(stderr, "pa2: frame %u part %p %s\n", pe_host_frame_number(), (const void *)part,
                ok ? "full precision" : "integer");
        last[i] = (signed char)ok;
    }
}

void pe_host_pa_project(const int *view, const int *part) {
    const FMat *f;
    FMat cam, out;
    int i;
    if (LOG_PA2) {
        log_status(part, lookup(part) != NULL);
    }
    if (!pe_host_graphics_is_hd() || NO_PRECISE_ANIM || !(f = lookup(part))) {
        Psyz_PgxpClearFloatMatrix();
        return;
    }
    for (i = 0; i < 3; i++) {
        if ((short)part[5 + i] != part[5 + i]) { /* the game wraps it to 16 bits */
            Psyz_PgxpClearFloatMatrix();
            return;
        }
    }
    from_int(view, &cam);
    mul_rot(cam.r, f->r, out.r);
    apply(cam.r, cam.t, f->t, out.t);
    if (LOG_PA) { /* debug: the game's composed matrix vs ours */
        static int n;
        FMat bi, gi;
        from_int(part, &bi);
        from_int((const int *)0x1F800000, &gi);
        if (n++ < 12) {
            fprintf(stderr, "pa: part r %.0f %.0f %.0f / %.1f %.1f %.1f t %.1f %.1f %.1f / %.2f %.2f %.2f\n",
                    bi.r[0], bi.r[1], bi.r[4], f->r[0], f->r[1], f->r[4], bi.t[0], bi.t[1], bi.t[2],
                    f->t[0], f->t[1], f->t[2]);
            fprintf(stderr, "pa: cam  r %.0f %.0f %.0f / %.1f %.1f %.1f t %.1f %.1f %.1f / %.2f %.2f %.2f\n",
                    gi.r[0], gi.r[1], gi.r[4], out.r[0], out.r[1], out.r[4], gi.t[0], gi.t[1], gi.t[2],
                    out.t[0], out.t[1], out.t[2]);
        }
    }
    Psyz_PgxpSetFloatMatrix(out.r, out.t);
}

void pe_host_pa_end(void) { Psyz_PgxpClearFloatMatrix(); }

/* Exact positions by vertex, for the polygons. Render_TransformMorphVertices
 * stores each projected vertex's screen XY in a table by vertex number
 * (D_800B1644), and Render_DrawTexturedQuads copies them from there into the
 * model's polygons. Looking an exact position up by its rounded screen XY
 * mixes up vertices that round to the same pixel (another model's, or this
 * one's), which made parts jump now and then; so the exact position is kept
 * by vertex here, and each polygon corner is tagged with it where the
 * polygon keeps it (Psyz_PgxpTag). */
#define VERTS 8192
static struct {
    unsigned sxy;
    int ok;
    float x, y, z;
} g_vert[VERTS];

static int g_vert_max; /* debug (PE_LOG_VERTS) */

/* RTPT stored its three screen XY at screen[0..2]; the table starts at
 * table + 3 (D_800B1638 + 3 = D_800B1644). */
void pe_host_pa_stored(const unsigned *screen, const unsigned *table) {
    int i = (int)(screen - (table + 3)), k;
    if (i + 3 > g_vert_max) {
        g_vert_max = i + 3;
    }
    for (k = 0; k < 3; k++, i++) {
        if (i < 0 || i >= VERTS) {
            continue;
        }
        g_vert[i].sxy = screen[k];
        g_vert[i].ok = Psyz_PgxpLast(k, screen[k], &g_vert[i].x, &g_vert[i].y, &g_vert[i].z);
    }
}

/* The game's back-face test of a model face (GTE NCLIP on the three corners'
 * whole-pixel screen XY, *area > 0 keeps the face): with all three exact
 * positions known, the sign comes from them instead. On faces of a pixel or
 * two the whole pixels often give the wrong sign, so faces that should be
 * hidden (a face seen from behind a head) showed through as specks at HD;
 * PGXP's culling correction does the same. xy is the table the corners came
 * from. */
void pe_host_pa_nclip(int *area, const unsigned *xy, int a, int b, int c) {
    float x0, y0, x1, y1, x2, y2, f;
    if (a < 0 || b < 0 || c < 0 || a >= VERTS || b >= VERTS || c >= VERTS || !g_vert[a].ok ||
        !g_vert[b].ok || !g_vert[c].ok || g_vert[a].sxy != xy[a] || g_vert[b].sxy != xy[b] ||
        g_vert[c].sxy != xy[c] || NO_PRECISE_NCLIP) {
        return;
    }
    x0 = g_vert[a].x, y0 = g_vert[a].y;
    x1 = g_vert[b].x, y1 = g_vert[b].y;
    x2 = g_vert[c].x, y2 = g_vert[c].y;
    f = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
    *area = f > 0.0f ? (f < 1.0f ? 1 : (int)f) : (f > -1.0f ? (f < 0.0f ? -1 : 0) : (int)f);
}

/* Debug: PE_LOG_VERTS=file writes, for every model drawn, its vertices'
 * whole-pixel and exact screen positions (one line per model and frame). */
void pe_host_pa_debug_model(const void *entity) {
    static FILE *f;
    static int init;
    int i;
    if (!init) {
        init = 1;
        if (getenv("PE_LOG_VERTS")) {
            f = fopen(getenv("PE_LOG_VERTS"), "w");
        }
    }
    if (f) {
        fprintf(f, "%u %p %d", pe_host_frame_number(), entity, g_vert_max);
        for (i = 0; i < g_vert_max && i < VERTS; i++) {
            int sx = (short)(g_vert[i].sxy & 0xFFFF), sy = (short)(g_vert[i].sxy >> 16);
            if (g_vert[i].ok) {
                fprintf(f, " %d,%d,%.3f,%.3f", sx, sy, g_vert[i].x, g_vert[i].y);
            } else {
                fprintf(f, " %d,%d,,", sx, sy);
            }
        }
        fputc('\n', f);
    }
    g_vert_max = 0;
}

/* A polygon corner at word got vertex index's screen XY from the table. */
void pe_host_pa_corner(const unsigned *word, int index) {
    if (LOG_PA3) { /* debug */
        static int n;
        if (n++ < 6) {
            fprintf(stderr, "pa3: corner %p idx %d word %08x vert ok %d sxy %08x\n", (const void *)word,
                    index, *word, index >= 0 && index < VERTS ? g_vert[index].ok : -1,
                    index >= 0 && index < VERTS ? g_vert[index].sxy : 0);
        }
    }
    if (index >= 0 && index < VERTS && g_vert[index].ok && g_vert[index].sxy == *word) {
        Psyz_PgxpTag(word, *word, g_vert[index].x, g_vert[index].y, g_vert[index].z);
    }
}
