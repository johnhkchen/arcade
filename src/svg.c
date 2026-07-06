#include "svg.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// A compact SVG rasterizer. Every shape reduces to one or more polylines
// (subpaths); we fill with an even-odd scanline and stroke as fattened
// segments + round joins. Everything is drawn into an RGBA buffer at SS× the
// requested size, then box-downsampled by ImageResize for cheap anti-aliasing.

#define SVG_SS       3            // supersample factor
#define SVG_MAXPTS   4096
#define SVG_MAXSUB   96

typedef struct { float x, y; } Pt;
typedef struct {                  // a shape flattened to device-space polylines
    Pt  p[SVG_MAXPTS];
    int start[SVG_MAXSUB + 1];    // start index of each subpath (+ final sentinel)
    int nsub, n;
} Poly;

typedef struct {                  // viewBox -> device transform + the target buffer
    unsigned char *px;
    int W, H;                     // device (supersampled) dimensions
    float ox, oy, sx, sy;         // p_dev = (p_view - (minx,miny)) * scale
} Ctx;

// ---- pixel blend (straight-alpha src-over) ------------------------------
static void Blend(Ctx *c, int x, int y, Color col)
{
    if (x < 0 || y < 0 || x >= c->W || y >= c->H || col.a == 0) return;
    unsigned char *p = c->px + ((size_t)y * c->W + x) * 4;
    float a = col.a / 255.0f, ia = 1.0f - a;
    p[0] = (unsigned char)(col.r * a + p[0] * ia);
    p[1] = (unsigned char)(col.g * a + p[1] * ia);
    p[2] = (unsigned char)(col.b * a + p[2] * ia);
    p[3] = (unsigned char)(col.a    + p[3] * ia);
}

// ---- poly building ------------------------------------------------------
static void PolyReset(Poly *q) { q->nsub = 0; q->n = 0; q->start[0] = 0; }
static void SubBegin(Poly *q)
{
    if (q->nsub >= SVG_MAXSUB) return;
    q->start[q->nsub] = q->n;                     // provisional; closed by SubEnd
}
static void SubEnd(Poly *q)
{
    if (q->nsub >= SVG_MAXSUB) return;
    if (q->n > q->start[q->nsub]) { q->nsub++; q->start[q->nsub] = q->n; }
}
static void Vtx(Ctx *c, Poly *q, float vx, float vy)   // append a viewBox-space vertex
{
    if (q->n >= SVG_MAXPTS) return;
    q->p[q->n++] = (Pt){ c->ox + vx * c->sx, c->oy + vy * c->sy };
}

// ---- even-odd scanline fill ---------------------------------------------
static int CmpF(const void *a, const void *b)
{ float d = *(const float *)a - *(const float *)b; return (d > 0) - (d < 0); }

static void Fill(Ctx *c, Poly *q, Color col)
{
    if (q->nsub == 0 || col.a == 0) return;
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < q->n; i++) { ymin = fminf(ymin, q->p[i].y); ymax = fmaxf(ymax, q->p[i].y); }
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    if (y0 < 0) y0 = 0; if (y1 > c->H) y1 = c->H;
    float xs[SVG_MAXSUB * 4 + 8];
    for (int y = y0; y < y1; y++) {
        float yc = y + 0.5f;
        int nx = 0;
        for (int s = 0; s < q->nsub; s++) {
            int a = q->start[s], b = q->start[s + 1];
            for (int i = a; i < b; i++) {
                Pt p0 = q->p[i], p1 = q->p[(i + 1 == b) ? a : i + 1];   // closed edge
                if ((p0.y <= yc) == (p1.y <= yc)) continue;
                float t = (yc - p0.y) / (p1.y - p0.y);
                if (nx < (int)(sizeof xs / sizeof xs[0])) xs[nx++] = p0.x + t * (p1.x - p0.x);
            }
        }
        if (nx < 2) continue;
        qsort(xs, nx, sizeof xs[0], CmpF);
        for (int i = 0; i + 1 < nx; i += 2) {
            int xa = (int)ceilf(xs[i] - 0.5f), xb = (int)floorf(xs[i + 1] - 0.5f);
            for (int x = xa; x <= xb; x++) Blend(c, x, y, col);
        }
    }
}

// ---- stroke (fattened segments + round joins) ---------------------------
static void Disk(Ctx *c, float cx, float cy, float r, Color col)
{
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy; if (fabsf(dy) > r) continue;
        float hw = sqrtf(r * r - dy * dy);
        int xa = (int)ceilf(cx - hw - 0.5f), xb = (int)floorf(cx + hw - 0.5f);
        for (int x = xa; x <= xb; x++) Blend(c, x, y, col);
    }
}
static void Segment(Ctx *c, Pt a, Pt b, float hw, Color col)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * hw, ny = dx / len * hw;
    Poly quad; PolyReset(&quad); SubBegin(&quad);
    quad.p[quad.n++] = (Pt){ a.x + nx, a.y + ny };
    quad.p[quad.n++] = (Pt){ b.x + nx, b.y + ny };
    quad.p[quad.n++] = (Pt){ b.x - nx, b.y - ny };
    quad.p[quad.n++] = (Pt){ a.x - nx, a.y - ny };
    SubEnd(&quad);
    Fill(c, &quad, col);
}
static void Stroke(Ctx *c, Poly *q, Color col, float w, bool closed)
{
    float hw = w * 0.5f;
    for (int s = 0; s < q->nsub; s++) {
        int a = q->start[s], b = q->start[s + 1];
        for (int i = a; i + 1 < b; i++) Segment(c, q->p[i], q->p[i + 1], hw, col);
        if (closed && b - a > 2) Segment(c, q->p[b - 1], q->p[a], hw, col);
        for (int i = a; i < b; i++) Disk(c, q->p[i].x, q->p[i].y, hw, col);   // joins/caps
    }
}

// ---- attribute + value parsing ------------------------------------------
static bool GetAttr(const char *s, const char *e, const char *name, char *out, int cap)
{
    size_t nl = strlen(name);
    for (const char *p = s; p + nl < e; p++) {
        if (strncmp(p, name, nl) != 0) continue;
        if (p != s && (isalnum((unsigned char)p[-1]) || p[-1] == '-')) continue;   // not a prefix match
        const char *q = p + nl; while (q < e && isspace((unsigned char)*q)) q++;
        if (q >= e || *q != '=') continue;
        q++; while (q < e && isspace((unsigned char)*q)) q++;
        char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
        int i = 0;
        while (q < e && i < cap - 1 && (quote ? *q != quote : !isspace((unsigned char)*q) && *q != '>')) out[i++] = *q++;
        out[i] = 0;
        return true;
    }
    return false;
}
static float AttrF(const char *s, const char *e, const char *name, float def)
{
    char buf[64];
    return GetAttr(s, e, name, buf, sizeof buf) ? (float)atof(buf) : def;
}

static int HexN(char c)
{ c = tolower(c); return c <= '9' ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0; }

// Parse a fill/stroke value into a color; returns false for "none"/missing.
static bool ParseColor(const char *s, const char *e, const char *name, float opacity, Color *out)
{
    char v[48];
    if (!GetAttr(s, e, name, v, sizeof v)) return false;
    if (strcmp(v, "none") == 0 || !v[0]) return false;
    Color c = { 0, 0, 0, 255 };
    if (v[0] == '#') {
        if (strlen(v) >= 7)      { c.r = HexN(v[1])*16+HexN(v[2]); c.g = HexN(v[3])*16+HexN(v[4]); c.b = HexN(v[5])*16+HexN(v[6]); }
        else if (strlen(v) >= 4) { c.r = HexN(v[1])*17; c.g = HexN(v[2])*17; c.b = HexN(v[3])*17; }
    } else if (!strcmp(v, "white"))  c = (Color){255,255,255,255};
    else if (!strcmp(v, "black"))    c = (Color){0,0,0,255};
    else if (!strcmp(v, "red"))      c = (Color){220,40,40,255};
    else if (!strcmp(v, "green"))    c = (Color){40,170,70,255};
    else if (!strcmp(v, "blue"))     c = (Color){40,80,210,255};
    else if (!strcmp(v, "yellow"))   c = (Color){240,210,50,255};
    c.a = (unsigned char)(255 * opacity);
    *out = c;
    return true;
}

static bool ReadNum(const char **pp, const char *e, float *out)
{
    const char *p = *pp;
    while (p < e && (isspace((unsigned char)*p) || *p == ',')) p++;
    if (p >= e || (!isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.')) return false;
    char *end; *out = strtof(p, &end);
    if (end == p) return false;
    *pp = end;
    return true;
}

// ---- shape flatteners ---------------------------------------------------
static void FlattenCubic(Ctx *c, Poly *q, float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3)
{
    const int N = 18;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / N, u = 1 - t;
        float x = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        Vtx(c, q, x, y);
    }
}
static void FlattenQuad(Ctx *c, Poly *q, float x0,float y0,float x1,float y1,float x2,float y2)
{
    const int N = 14;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / N, u = 1 - t;
        Vtx(c, q, u*u*x0 + 2*u*t*x1 + t*t*x2, u*u*y0 + 2*u*t*y1 + t*t*y2);
    }
}
static void BuildEllipse(Ctx *c, Poly *q, float cx, float cy, float rx, float ry)
{
    PolyReset(q); SubBegin(q);
    for (int i = 0; i < 72; i++) { float a = i * (2*PI/72); Vtx(c, q, cx + cosf(a)*rx, cy + sinf(a)*ry); }
    SubEnd(q);
}
static void BuildRect(Ctx *c, Poly *q, float x, float y, float w, float h, float rx)
{
    PolyReset(q); SubBegin(q);
    if (rx <= 0) {
        Vtx(c,q,x,y); Vtx(c,q,x+w,y); Vtx(c,q,x+w,y+h); Vtx(c,q,x,y+h);
    } else {
        if (rx > w/2) rx = w/2; if (rx > h/2) rx = h/2;
        const int A = 8;
        float corners[4][3] = { {x+w-rx, y+rx, -PI/2}, {x+w-rx, y+h-rx, 0}, {x+rx, y+h-rx, PI/2}, {x+rx, y+rx, PI} };
        for (int k = 0; k < 4; k++)
            for (int i = 0; i <= A; i++) {
                float a = corners[k][2] + (float)i/A * (PI/2);
                Vtx(c, q, corners[k][0] + cosf(a)*rx, corners[k][1] + sinf(a)*rx);
            }
    }
    SubEnd(q);
}
static void BuildPoints(Ctx *c, Poly *q, const char *s, const char *e)   // polygon/polyline
{
    PolyReset(q); SubBegin(q);
    const char *p = s; float x, y;
    while (ReadNum(&p, e, &x) && ReadNum(&p, e, &y)) Vtx(c, q, x, y);
    SubEnd(q);
}
static void BuildPath(Ctx *c, Poly *q, const char *d, const char *e)
{
    PolyReset(q);
    float cx = 0, cy = 0, sx = 0, sy = 0;      // current point, subpath start
    bool open = false;
    const char *p = d;
    char cmd = 0;
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= e) break;
        if (isalpha((unsigned char)*p)) cmd = *p++;
        bool rel = islower((unsigned char)cmd);
        float a, b, c1, c2, c3, c4, c5, c6;
        switch (toupper(cmd)) {
        case 'M':
            if (!ReadNum(&p, e, &a) || !ReadNum(&p, e, &b)) { p = e; break; }
            if (open) SubEnd(q);
            cx = rel ? cx + a : a; cy = rel ? cy + b : b; sx = cx; sy = cy;
            SubBegin(q); Vtx(c, q, cx, cy); open = true;
            cmd = rel ? 'l' : 'L';   // subsequent pairs are implicit lineto
            break;
        case 'L':
            if (!ReadNum(&p, e, &a) || !ReadNum(&p, e, &b)) { p = e; break; }
            cx = rel ? cx + a : a; cy = rel ? cy + b : b; Vtx(c, q, cx, cy);
            break;
        case 'H':
            if (!ReadNum(&p, e, &a)) { p = e; break; }
            cx = rel ? cx + a : a; Vtx(c, q, cx, cy);
            break;
        case 'V':
            if (!ReadNum(&p, e, &a)) { p = e; break; }
            cy = rel ? cy + a : a; Vtx(c, q, cx, cy);
            break;
        case 'C':
            if (!ReadNum(&p,e,&c1)||!ReadNum(&p,e,&c2)||!ReadNum(&p,e,&c3)||!ReadNum(&p,e,&c4)||!ReadNum(&p,e,&c5)||!ReadNum(&p,e,&c6)) { p=e; break; }
            if (rel) { c1+=cx;c2+=cy;c3+=cx;c4+=cy;c5+=cx;c6+=cy; }
            FlattenCubic(c, q, cx, cy, c1, c2, c3, c4, c5, c6); cx = c5; cy = c6;
            break;
        case 'Q':
            if (!ReadNum(&p,e,&c1)||!ReadNum(&p,e,&c2)||!ReadNum(&p,e,&c3)||!ReadNum(&p,e,&c4)) { p=e; break; }
            if (rel) { c1+=cx;c2+=cy;c3+=cx;c4+=cy; }
            FlattenQuad(c, q, cx, cy, c1, c2, c3, c4); cx = c3; cy = c4;
            break;
        case 'Z':
            if (open) { SubEnd(q); open = false; } cx = sx; cy = sy;
            break;
        default: p = e; break;   // unsupported command -> stop
        }
    }
    if (open) SubEnd(q);
}

// ---- element dispatch ---------------------------------------------------
static void PaintShape(Ctx *c, Poly *q, const char *s, const char *e, bool fillDefault, bool closed)
{
    float op = AttrF(s, e, "opacity", 1.0f);
    Color col;
    char fbuf[8];
    bool hasFillAttr = GetAttr(s, e, "fill", fbuf, sizeof fbuf);
    if (ParseColor(s, e, "fill", op * AttrF(s, e, "fill-opacity", 1.0f), &col)) Fill(c, q, col);
    else if (fillDefault && !hasFillAttr) { col = (Color){0,0,0,(unsigned char)(255*op)}; Fill(c, q, col); }
    if (ParseColor(s, e, "stroke", op * AttrF(s, e, "stroke-opacity", 1.0f), &col)) {
        float w = AttrF(s, e, "stroke-width", 1.0f) * (c->sx + c->sy) * 0.5f;
        Stroke(c, q, col, w, closed);
    }
}

Image SvgRasterize(const char *svg, int outW, int outH)
{
    Ctx c = {0};
    c.W = outW * SVG_SS; c.H = outH * SVG_SS;
    c.px = (unsigned char *)calloc((size_t)c.W * c.H, 4);
    c.ox = 0; c.oy = 0; c.sx = c.W / 100.0f; c.sy = c.H / 100.0f;   // default viewBox 0 0 100 100

    Poly q;
    for (const char *p = svg; (p = strchr(p, '<')); ) {
        p++;
        if (*p == '/' || *p == '!' || *p == '?') { const char *g = strchr(p, '>'); if (!g) break; p = g + 1; continue; }
        const char *ns = p; while (isalnum((unsigned char)*p) || *p == '-') p++;
        int nl = (int)(p - ns); char name[16]; if (nl > 15) nl = 15; memcpy(name, ns, nl); name[nl] = 0;
        const char *as = p, *ae = strchr(p, '>'); if (!ae) break;

        if (!strcmp(name, "svg")) {
            char vb[96];
            if (GetAttr(as, ae, "viewBox", vb, sizeof vb)) {
                const char *v = vb, *ve = vb + strlen(vb); float mnx, mny, vw, vh;
                if (ReadNum(&v, ve, &mnx) && ReadNum(&v, ve, &mny) && ReadNum(&v, ve, &vw) && ReadNum(&v, ve, &vh) && vw > 0 && vh > 0) {
                    c.sx = c.W / vw; c.sy = c.H / vh; c.ox = -mnx * c.sx; c.oy = -mny * c.sy;
                }
            }
        } else if (!strcmp(name, "rect")) {
            BuildRect(&c, &q, AttrF(as,ae,"x",0), AttrF(as,ae,"y",0), AttrF(as,ae,"width",0), AttrF(as,ae,"height",0), AttrF(as,ae,"rx",0));
            PaintShape(&c, &q, as, ae, true, true);
        } else if (!strcmp(name, "circle")) {
            float r = AttrF(as,ae,"r",0); BuildEllipse(&c, &q, AttrF(as,ae,"cx",0), AttrF(as,ae,"cy",0), r, r);
            PaintShape(&c, &q, as, ae, true, true);
        } else if (!strcmp(name, "ellipse")) {
            BuildEllipse(&c, &q, AttrF(as,ae,"cx",0), AttrF(as,ae,"cy",0), AttrF(as,ae,"rx",0), AttrF(as,ae,"ry",0));
            PaintShape(&c, &q, as, ae, true, true);
        } else if (!strcmp(name, "polygon") || !strcmp(name, "polyline")) {
            char pts[1024];
            if (GetAttr(as, ae, "points", pts, sizeof pts)) {
                BuildPoints(&c, &q, pts, pts + strlen(pts));
                PaintShape(&c, &q, as, ae, name[4] == 'g', name[4] == 'g');   // polygon fills+closes; polyline neither
            }
        } else if (!strcmp(name, "path")) {
            char *d = (char *)malloc(4096);
            if (d && GetAttr(as, ae, "d", d, 4096)) {
                BuildPath(&c, &q, d, d + strlen(d));
                PaintShape(&c, &q, as, ae, true, true);
            }
            free(d);
        } else if (!strcmp(name, "line")) {
            PolyReset(&q); SubBegin(&q);
            Vtx(&c, &q, AttrF(as,ae,"x1",0), AttrF(as,ae,"y1",0));
            Vtx(&c, &q, AttrF(as,ae,"x2",0), AttrF(as,ae,"y2",0));
            SubEnd(&q);
            PaintShape(&c, &q, as, ae, false, false);
        }
        p = ae + 1;
    }

    Image img = { c.px, c.W, c.H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    ImageResize(&img, outW, outH);   // box/bicubic downsample -> anti-aliased
    return img;
}

Texture2D SvgTexture(const char *svg, int outW, int outH)
{
    Image img = SvgRasterize(svg, outW, outH);
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
    return tex;
}
