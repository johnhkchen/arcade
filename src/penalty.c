// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core.
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "ballstrike.h"
#include "jumbotron.h"
#include "svg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

#define GOAL_Z         11.0f   /* ~12 yds — regulation penalty distance */
#define GOAL_HALF_W     3.66f
#define GOAL_H          2.44f
#define BALL_R          0.11f
#define POST_R          0.06f
#define NET_DEPTH       1.4f
#define KICKS_TOTAL     5
#define BACK_WALL      (GOAL_Z + 2.1f)   // stops balls before the stands (no through-fans)

#define KPR_DIVE_SPEED  8.5f
#define KPR_SHUF_SPEED  12.0f   // low lateral jolt is faster & flatter than the leaping dive
#define KPR_HOME_Y      1.05f
#define KPR_BODY        0.42f
#define GLOVE_R         0.17f
#define ARM             0.80f
#define SHOULDER        0.32f
#define SHOULDER_Y      0.25f
#define REACT_DUR       3.0f
#define SCRAMBLE_MAX    2
#define SCRAMBLE_REACH  3.7f
#define SCRAMBLE_SPEED  9.5f

#define CAM_POS        (Vector3){ 0.0f, 2.6f, -6.5f }
#define CAM_TGT        (Vector3){ 0.0f, 1.4f, GOAL_Z }

#define NET_NX         17
#define NET_NY         11
#define BOARD_Y         6.2f
#define BOARD_Z        (GOAL_Z + 8.0f)
#define BOARD_HW        6.0f
#define BOARD_HH        1.7f

typedef enum { PHASE_AIM, PHASE_CHARGE, PHASE_FLY, PHASE_RESULT } Phase;
typedef enum { RES_NONE, RES_GOAL, RES_SAVE, RES_POST, RES_MISS } Result;
typedef struct { Vector3 p, prev; bool pin; } NetNode;

typedef struct {
    Phase   phase;
    float   yaw, pitch, curve, power01, chargeDir;
    Ball    ball;
    // keeper
    Vector3 kprPos, kprVel;
    float   kprReact, kprDiveX, kprDiveH;
    bool    kprLaunched;
    int     kprMove;                 // 0 = leaping dive, 1 = low lateral shuffle
    int     scrambleCount;
    float   scrambleCd;
    // keeper visual pose (sprung)
    Vector3 kprAxis, kprGL, kprGR, kprGLv, kprGRv, kprHeadOff, reactBase, reactAxis;
    int     reactAnim;
    bool    reactCelebrate, reactOnGround, landed, reactHeld;
    float   reactT;
    // flow
    int     kick, scored, round, target;
    Result  result, kickRes[KICKS_TOTAL];
    bool    resolved, caught, hitWood, roundWon, gameWon, gameLost;
    float   resultTimer, celebrate, flight;
    // touch input
    bool    touchCharge;
    float   pressX;
    // bullet-time replay
    bool    cine, cineArmed;
    float   cineT, cineDur, cineAng0, cineDir, closest;
    Vector3 cineFocus;
    Camera3D cam;
} Game;

static Game    g;
static NetNode gNet[NET_NY][NET_NX];
static float   gNetDx, gNetDy;

// ---- baked art: the ball skin, and the keeper's shaded head + expression decals ----
#define FACE_SKINS 4
enum { EXPR_FOCUS, EXPR_WORRY, EXPR_HAPPY, EXPR_ANGRY, EXPR_COUNT };
static Texture2D  gBallTex;
static Model      gBallModel;
static Quaternion gBallRot;                 // accumulated tumble so the skin rolls
static Texture2D  gHeadTex[FACE_SKINS];     // shaded skin sphere + hair cap (one per skin)
static Model      gHeadModel;               // reused; retextured per keeper
static Texture2D  gExprTex[EXPR_COUNT];     // transparent face features, one per emotion
static int        gFaceSel;                 // which skin this keeper wears
static const Color gSkinTone[FACE_SKINS] = {
    {247,206,168,255}, {225,170,120,255}, {171,116,74,255}, {112,76,52,255},
};
static const Color gHairTone[FACE_SKINS] = {
    {74,48,28,255}, {32,24,20,255}, {26,20,17,255}, {20,15,13,255},
};

typedef struct { Color a, b; } Team;   // primary, secondary kit colors
static const Team TEAMS[] = {
    {{247,203,44,255},{0,120,60,255}},   {{108,172,228,255},{245,245,245,255}},   // 0 canary/green   1 sky/white
    {{40,60,140,255},{205,30,45,255}},   {{224,60,50,255},{245,245,245,255}},     // 2 blue/red       3 red/white
    {{255,120,20,255},{25,25,25,255}},   {{25,25,30,255},{230,230,230,255}},      // 4 orange/black   5 black/white
    {{0,90,160,255},{240,220,40,255}},   {{150,20,50,255},{240,240,240,255}},     // 6 blue/gold      7 maroon/white
    {{245,245,245,255},{28,48,120,255}}, {{20,85,165,255},{245,245,245,255}},     // 8 white/navy     9 azzurri
    {{170,30,45,255},{245,215,70,255}},  {{210,40,55,255},{40,60,150,255}},       // 10 red/gold      11 red/navy
    {{200,40,45,255},{250,205,50,255}},  {{90,175,215,255},{25,40,90,255}},       // 12 red/yellow    13 sky/navy
};
static Team gHome, gAway;
static BoardEra gEra;
#define ROUNDS 3
// A historical shootout: its era look, year, how many the OPPONENT scored, and the two kits.
typedef struct { BoardEra era; const char *year; int oppGoals; int home, away; } Moment;
// A deep pool; each session draws ROUNDS of these at random, so the run of
// eras / matchups / targets differs every time you sit down.
static const Moment POOL[] = {
    { ERA_70S,    "1970", 3, 0,  9 },   { ERA_70S,    "1974", 2, 5,  4 },
    { ERA_70S,    "1978", 3, 1,  3 },   { ERA_90S,    "1990", 2, 5,  1 },
    { ERA_90S,    "1994", 4, 0,  9 },   { ERA_90S,    "1998", 3, 2,  0 },
    { ERA_00S,    "2006", 4, 2,  9 },   { ERA_00S,    "2010", 2, 10, 13 },
    { ERA_MODERN, "2018", 3, 2, 11 },   { ERA_MODERN, "2022", 4, 1,  2 },
};
static Moment gRounds[ROUNDS];

// ---- in-universe sponsors: the b28 product family on pitch-side hoardings ----
typedef struct { const char *name, *tag; Color bg, fg, accent; } Sponsor;
#define NSPON 6
static const Sponsor SPONSORS[NSPON] = {
    { "b28.dev",    "make yourself at home",   { 68,103,155,255}, {250,248,245,255}, {242,239,233,255} },
    { "LISA",       "runs your coding agents", { 20, 24, 32,255}, {120,220,140,255}, { 90,180,110,255} },
    { "vend",       "intent to backlog",       {228,120, 42,255}, { 24, 20, 16,255}, {255,222,120,255} },
    { "PLANTASTIC", "plan a garden",           { 28,118, 70,255}, {240,246,236,255}, {150,212,120,255} },
    { "CONSECUTIVE", "five in a row",          { 22,108,112,255}, {240,246,244,255}, {245,210, 90,255} },
    { "ROWCLEAR",   "falling blocks",          { 62, 42,112,255}, {245,240,255,255}, {170,122,240,255} },
};
static Texture2D gBannerTex[NSPON];

static float Frand(void)  { return GetRandomValue(0, 1000) / 1000.0f; }
static float Frand2(void) { return (Frand() - 0.5f) * 2.0f; }

// ---- baked art ----------------------------------------------------------
// A classic black-pentagon football, authored as an equirectangular SVG so it
// wraps a UV sphere. Pentagons near the poles are widened to fight uv stretch.
static void BuildBallSvg(char *b)
{
    int o = sprintf(b, "<svg viewBox='0 0 360 180'>"
                       "<rect x='0' y='0' width='360' height='180' fill='#eef0ec'/>");
    static const float C[][3] = {
        { 40,45,10},{130,55,-20},{220,48,30},{310,52, 0},
        { 80,95,15},{175,100,-10},{270,95,25},{350,98, 5},
        { 20,140,-15},{115,150,20},{210,145,-25},{300,148,10},
        {180,16,0},{180,164,0},
    };
    for (int i = 0; i < (int)(sizeof(C)/sizeof(C[0])); i++) {
        float lon = C[i][0], lat = C[i][1], rot = C[i][2]*DEG2RAD;
        float latf = sinf(lat*DEG2RAD); if (latf < 0.34f) latf = 0.34f;
        float rx = fminf(15.0f/latf, 46.0f), ry = 15.0f;
        o += sprintf(b+o, "<polygon points='");
        for (int k = 0; k < 5; k++) {
            float a = rot + k*(2*PI/5) - PI/2;
            o += sprintf(b+o, "%.1f,%.1f ", lon + cosf(a)*rx, lat + sinf(a)*ry);
        }
        o += sprintf(b+o, "' fill='#16181c'/>");
    }
    sprintf(b+o, "</svg>");
}
// A shaded skin head with a hair cap. In raylib's sphere UVs, texcoord.x is the
// polar angle from the +Z pole and texcoord.y is azimuth — so hair and shading
// key on the COLUMN only (azimuth-invariant): a clean cap around +Z, no seam.
// We then aim the model's +Z at "up", putting the cap on the crown.
static Image BuildHeadImage(Color skin, Color hair)
{
    const int W = 192, H = 32;
    unsigned char *px = (unsigned char *)malloc((size_t)W*H*4);
    for (int x = 0; x < W; x++) {
        float polar = (float)x / (W - 1);                    // 0 = crown (+Z), 1 = neck (-Z)
        float shade = Clamp(1.13f - polar*0.5f - fmaxf(0.0f, polar-0.8f)*1.4f, 0.55f, 1.13f);
        Color c;
        if (polar < 0.30f) {                                 // hair cap over the crown
            float hs = Clamp(1.06f - polar*0.7f, 0.72f, 1.06f);
            c = (Color){ (unsigned char)(hair.r*hs), (unsigned char)(hair.g*hs), (unsigned char)(hair.b*hs), 255 };
        } else {
            c = (Color){ (unsigned char)fminf(skin.r*shade,255), (unsigned char)fminf(skin.g*shade,255),
                         (unsigned char)fminf(skin.b*shade,255), 255 };
        }
        for (int y = 0; y < H; y++) { unsigned char *p = px + ((size_t)y*W + x)*4; p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=255; }
    }
    return (Image){ px, W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
}
// An expression: crisp SVG features on a transparent field, composited over the
// shaded head. Brows/eyes/mouth are swapped by emotion; a soft socket adds depth.
static void BuildExprSvg(char *b, int expr)
{
    int o = sprintf(b, "<svg viewBox='0 0 100 100'>");
    o += sprintf(b+o, "<ellipse cx='35' cy='49' rx='12' ry='12' fill='#00000014'/>");   // soft sockets
    o += sprintf(b+o, "<ellipse cx='65' cy='49' rx='12' ry='12' fill='#00000014'/>");
    const char *browL, *browR, *mouth; float ey, eyry, pdy;
    switch (expr) {
    case EXPR_WORRY:                                                                    // brows up-inner, wide eyes
        browL = "<path d='M23,37 L45,32' stroke='#2a2620' stroke-width='5'/>";
        browR = "<path d='M55,32 L77,37' stroke='#2a2620' stroke-width='5'/>";
        mouth = "<ellipse cx='50' cy='73' rx='6' ry='7' fill='#4a1c18'/>";
        ey = 49; eyry = 11.0f; pdy = 2.0f; break;
    case EXPR_HAPPY:                                                                    // arched brows, big smile
        browL = "<path d='M23,33 Q35,28 46,33' stroke='#2a2620' stroke-width='5' fill='none'/>";
        browR = "<path d='M54,33 Q65,28 77,33' stroke='#2a2620' stroke-width='5' fill='none'/>";
        mouth = "<path d='M33,68 Q50,86 67,68 Q50,76 33,68 Z' fill='#7a2a24'/>";
        ey = 48; eyry = 9.5f; pdy = -2.0f; break;
    case EXPR_ANGRY:                                                                    // furrowed brows, grimace
        browL = "<path d='M23,33 L46,42' stroke='#241f1a' stroke-width='6'/>";
        browR = "<path d='M54,42 L77,33' stroke='#241f1a' stroke-width='6'/>";
        mouth = "<path d='M34,77 Q50,69 66,77' stroke='#5a201c' stroke-width='5' fill='none'/>";
        ey = 51; eyry = 8.0f; pdy = 1.5f; break;
    default:            /* EXPR_FOCUS */                                               // level brows, steady eyes
        browL = "<path d='M23,35 L46,36' stroke='#2a2620' stroke-width='5'/>";
        browR = "<path d='M54,36 L77,35' stroke='#2a2620' stroke-width='5'/>";
        mouth = "<path d='M40,74 L60,74' stroke='#6a2620' stroke-width='4'/>";
        ey = 49; eyry = 10.0f; pdy = 1.5f; break;
    }
    o += sprintf(b+o, "%s%s", browL, browR);
    o += sprintf(b+o, "<ellipse cx='35' cy='%.1f' rx='9' ry='%.1f' fill='#ffffff'/>", ey, eyry);
    o += sprintf(b+o, "<ellipse cx='65' cy='%.1f' rx='9' ry='%.1f' fill='#ffffff'/>", ey, eyry);
    o += sprintf(b+o, "<circle cx='36' cy='%.1f' r='4.4' fill='#241c18'/>", ey+pdy);
    o += sprintf(b+o, "<circle cx='64' cy='%.1f' r='4.4' fill='#241c18'/>", ey+pdy);
    o += sprintf(b+o, "<circle cx='37.6' cy='%.1f' r='1.6' fill='#ffffff'/>", ey+pdy-1.9f);
    o += sprintf(b+o, "<circle cx='65.6' cy='%.1f' r='1.6' fill='#ffffff'/>", ey+pdy-1.9f);
    o += sprintf(b+o, "<path d='M50,53 L45,65 L55,65' fill='none' stroke='#00000022' stroke-width='2.5'/>");
    o += sprintf(b+o, "%s</svg>", mouth);
}
static Color Mix(Color a, Color b, float t)
{
    return (Color){ (unsigned char)(a.r + (b.r-a.r)*t), (unsigned char)(a.g + (b.g-a.g)*t),
                    (unsigned char)(a.b + (b.b-a.b)*t), 255 };
}
// A pitch-side hoarding with a b28 "app-icon" logo tile, a glossy graded board and
// a shadowed wordmark. Baked at 2x and downsampled so the pixel font reads clean.
static Texture2D BakeBanner(Sponsor s)
{
    const int S = 2, H = 128*S;
    Font f = GetFontDefault();
    int pad = 17*S, ts = H - 2*pad, tx = pad + ts + 20*S;          // logo tile then wordmark
    float nfs = 46*S, nsp = 3*S;
    Vector2 nz = MeasureTextEx(f, s.name, nfs, nsp);
    Vector2 tz = s.tag ? MeasureTextEx(f, s.tag, 20*S, 1*S) : (Vector2){0,0};
    const int W = tx + (int)fmaxf(nz.x, tz.x) + 24*S;              // size the board to fit its own text

    unsigned char *px = (unsigned char *)malloc((size_t)W*H*4);
    Color hi = Mix(s.bg, (Color){255,255,255,255}, 0.16f), lo = Mix(s.bg, (Color){0,0,0,255}, 0.34f);
    for (int y = 0; y < H; y++) {                                   // vertical gloss: sheen up top, shadow below
        float v = (float)y / (H-1);
        Color row = (v < 0.5f) ? Mix(hi, s.bg, v*2.0f) : Mix(s.bg, lo, (v-0.5f)*2.0f);
        for (int x = 0; x < W; x++) { unsigned char *p = px+((size_t)y*W+x)*4; p[0]=row.r; p[1]=row.g; p[2]=row.b; p[3]=255; }
    }
    Image im = { px, W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    ImageDrawRectangle(&im, 0, 0, W, 5*S, s.accent);               // top / bottom accent rails
    ImageDrawRectangle(&im, 0, H-5*S, W, 5*S, s.accent);

    ImageDrawRectangle(&im, pad, pad, ts, ts, Mix(s.accent, (Color){0,0,0,255}, 0.18f));   // logo tile (bevel)
    ImageDrawRectangle(&im, pad, pad, ts, ts/2, Mix(s.accent, (Color){255,255,255,255}, 0.12f));
    ImageDrawRectangle(&im, pad+2*S, pad+3*S, ts-4*S, ts-5*S, s.accent);
    char init[2] = { (char)(s.name[0]>='a'&&s.name[0]<='z' ? s.name[0]-32 : s.name[0]), 0 };
    float ifs = ts*0.6f; Vector2 iz = MeasureTextEx(f, init, ifs, 1);
    ImageDrawTextEx(&im, f, init, (Vector2){ pad + (ts-iz.x)/2, pad + (ts-iz.y)/2 }, ifs, 1, s.bg);

    float ny = (H - nz.y)/2 - 9*S;                                 // wordmark + soft shadow
    ImageDrawTextEx(&im, f, s.name, (Vector2){ tx+2*S, ny+3*S }, nfs, nsp, (Color){0,0,0,90});
    ImageDrawTextEx(&im, f, s.name, (Vector2){ tx, ny }, nfs, nsp, s.fg);
    ImageDrawRectangle(&im, tx, (int)(ny+nz.y+5*S), (int)nz.x, 3*S, s.accent);   // accent underline
    if (s.tag) ImageDrawTextEx(&im, f, s.tag, (Vector2){ tx, ny+nz.y+11*S }, 20*S, 1*S, Mix(s.fg, s.bg, 0.4f));

    ImageResize(&im, W/S, H/S);                                    // downsample -> crisp, aspect preserved
    Texture2D t = LoadTextureFromImage(im);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    UnloadImage(im);
    return t;
}
static void LoadArt(void)
{
    static char buf[8192];
    BuildBallSvg(buf);
    gBallTex   = SvgTexture(buf, 512, 256);
    gBallModel = LoadModelFromMesh(GenMeshSphere(BALL_R*3.0f, 22, 22));
    gBallModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = gBallTex;
    gBallRot   = QuaternionIdentity();

    gHeadModel = LoadModelFromMesh(GenMeshSphere(0.30f, 20, 24));
    for (int i = 0; i < FACE_SKINS; i++) {
        Image im = BuildHeadImage(gSkinTone[i], gHairTone[i]);
        gHeadTex[i] = LoadTextureFromImage(im); SetTextureFilter(gHeadTex[i], TEXTURE_FILTER_BILINEAR);
        UnloadImage(im);
    }
    for (int e = 0; e < EXPR_COUNT; e++) { BuildExprSvg(buf, e); gExprTex[e] = SvgTexture(buf, 176, 176); }
    for (int i = 0; i < NSPON; i++) gBannerTex[i] = BakeBanner(SPONSORS[i]);
}
// Draw a texture as a flat quad centered at `c`, facing `nrm`, with `up` as its
// vertical — used for the face decal so it turns to look wherever the ball is.
static void DrawDecal(Texture2D tex, Vector3 c, Vector3 nrm, Vector3 up, float w, float h)
{
    Vector3 right = Vector3CrossProduct(up, nrm);
    if (Vector3Length(right) < 1e-4f) return;
    right = Vector3Normalize(right);
    Vector3 vu = Vector3Normalize(Vector3CrossProduct(nrm, right));
    Vector3 hx = Vector3Scale(right, w*0.5f), hy = Vector3Scale(vu, h*0.5f);
    Vector3 tl = Vector3Add(Vector3Add(c, Vector3Negate(hx)), hy);
    Vector3 tr = Vector3Add(Vector3Add(c, hx), hy);
    Vector3 br = Vector3Subtract(Vector3Add(c, hx), hy);
    Vector3 bl = Vector3Subtract(Vector3Add(c, Vector3Negate(hx)), hy);
    rlDisableBackfaceCulling();
    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
        rlColor4ub(255,255,255,255);
        rlTexCoord2f(0,0); rlVertex3f(tl.x, tl.y, tl.z);
        rlTexCoord2f(0,1); rlVertex3f(bl.x, bl.y, bl.z);
        rlTexCoord2f(1,1); rlVertex3f(br.x, br.y, br.z);
        rlTexCoord2f(1,0); rlVertex3f(tr.x, tr.y, tr.z);
    rlEnd();
    rlSetTexture(0);
    rlEnableBackfaceCulling();
}
// Roll the ball skin: rolling contact tumbles it, sidespin adds visible curl.
static void SpinBall(float dt)
{
    Vector3 w = {0};
    float sp = Vector3Length(g.ball.vel);
    if (sp > 0.2f) {
        Vector3 axis = Vector3CrossProduct((Vector3){0,1,0}, g.ball.vel);
        if (Vector3Length(axis) > 1e-3f) w = Vector3Scale(Vector3Normalize(axis), sp/(BALL_R*3.0f));
    }
    w.y += g.ball.spin.y * 2.2f;
    float wl = Vector3Length(w);
    if (wl > 1e-4f) {
        Quaternion dq = QuaternionFromAxisAngle(Vector3Scale(w, 1.0f/wl), wl*dt);
        gBallRot = QuaternionNormalize(QuaternionMultiply(dq, gBallRot));
    }
}

// ---- net cloth ----------------------------------------------------------
static void InitNet(void)
{
    gNetDx = (2.0f*GOAL_HALF_W) / (NET_NX-1);
    gNetDy = GOAL_H / (NET_NY-1);
    for (int r = 0; r < NET_NY; r++)
        for (int c = 0; c < NET_NX; c++) {
            Vector3 rp = { -GOAL_HALF_W + c*gNetDx, r*gNetDy, GOAL_Z + NET_DEPTH };
            gNet[r][c].p = rp; gNet[r][c].prev = rp;
            gNet[r][c].pin = (r == 0 || r == NET_NY-1 || c == 0 || c == NET_NX-1);
        }
}
static void Relax(NetNode *a, NetNode *b, float rest)
{
    Vector3 d = Vector3Subtract(b->p, a->p);
    float len = Vector3Length(d);
    if (len < 1e-5f) return;
    Vector3 corr = Vector3Scale(d, 0.5f * (len - rest) / len);
    if (!a->pin) a->p = Vector3Add(a->p, corr);
    if (!b->pin) b->p = Vector3Subtract(b->p, corr);
}
static void StepNet(float dt)
{
    float dt2 = dt*dt;
    for (int r = 0; r < NET_NY; r++)
        for (int c = 0; c < NET_NX; c++) {
            NetNode *n = &gNet[r][c];
            if (n->pin) continue;
            Vector3 cur = n->p;
            Vector3 v = Vector3Scale(Vector3Subtract(n->p, n->prev), 0.985f);
            n->p = Vector3Add(n->p, v);
            n->p.y -= 9.5f * dt2;
            n->prev = cur;
        }
    bool inGoal = g.ball.pos.z > GOAL_Z - 0.3f && fabsf(g.ball.pos.x) < GOAL_HALF_W + 0.3f && g.ball.pos.y < GOAL_H + 0.3f;
    if (inGoal)
        for (int r = 0; r < NET_NY; r++)
            for (int c = 0; c < NET_NX; c++) {
                NetNode *n = &gNet[r][c];
                if (n->pin) continue;
                float dx = n->p.x - g.ball.pos.x, dy = n->p.y - g.ball.pos.y;
                if (dx*dx + dy*dy < 0.55f && g.ball.pos.z + BALL_R > n->p.z)
                    n->p.z += (g.ball.pos.z + BALL_R*1.4f - n->p.z) * 0.6f;
            }
    for (int it = 0; it < 2; it++)
        for (int r = 0; r < NET_NY; r++)
            for (int c = 0; c < NET_NX; c++) {
                if (c+1 < NET_NX) Relax(&gNet[r][c], &gNet[r][c+1], gNetDx * 1.06f);
                if (r+1 < NET_NY) Relax(&gNet[r][c], &gNet[r+1][c], gNetDy * 1.06f);
            }
}
static void DrawNet(void)
{
    Color nc = (Color){ 215, 220, 226, 70 };
    for (int r = 0; r < NET_NY; r++)
        for (int c = 0; c < NET_NX; c++) {
            if (c+1 < NET_NX) DrawLine3D(gNet[r][c].p, gNet[r][c+1].p, nc);
            if (r+1 < NET_NY) DrawLine3D(gNet[r][c].p, gNet[r+1][c].p, nc);
        }
}

// ---- keeper -------------------------------------------------------------
// The keeper's shoulder in his OWN (possibly diving) frame — arms hinge from the
// body as it tilts, not from a world-upright rig.
static Vector3 KprShoulder(int side)
{
    Vector3 up = (Vector3Length(g.kprAxis) > 0.1f) ? Vector3Normalize(g.kprAxis) : (Vector3){0,1,0};
    Vector3 sv = Vector3CrossProduct(up, (Vector3){0,0,1});
    if (Vector3Length(sv) < 0.1f) sv = (Vector3){1,0,0};
    sv = Vector3Normalize(sv);
    return Vector3Add(g.kprPos, Vector3Add(Vector3Scale(sv, side*SHOULDER), Vector3Scale(up, SHOULDER_Y)));
}
// Where the hands should point: the spot the keeper has committed his dive to,
// homing onto the actual ball only as it reaches the line. So the reach leads
// toward where he's going, then closes onto the ball for the touch.
static Vector3 SaveTarget(void)
{
    Vector3 commit = { g.kprDiveX, g.kprDiveH, GOAL_Z - 0.35f };
    float t = Clamp((g.ball.pos.z - (GOAL_Z - 3.2f)) / 3.2f, 0.0f, 1.0f);
    return Vector3Lerp(commit, g.ball.pos, t);
}
static Vector3 GlovePos(int side)
{
    Vector3 up = (Vector3Length(g.kprAxis) > 0.1f) ? Vector3Normalize(g.kprAxis) : (Vector3){0,1,0};
    Vector3 sv = Vector3CrossProduct(up, (Vector3){0,0,1});
    if (Vector3Length(sv) < 0.1f) sv = (Vector3){1,0,0};
    sv = Vector3Normalize(sv);
    Vector3 sh  = KprShoulder(side);
    Vector3 tgt = Vector3Add(SaveTarget(), Vector3Scale(sv, side*0.15f));   // keep hands shoulder-width, not clasped
    Vector3 to = Vector3Subtract(tgt, sh);
    float d = Vector3Length(to);
    if (d > ARM) to = Vector3Scale(to, ARM / d);
    return Vector3Add(sh, to);
}
static void ResetKeeper(void)
{
    g.kprPos = (Vector3){ 0.0f, KPR_HOME_Y, GOAL_Z - 0.5f };
    g.kprVel = (Vector3){0};
    g.kprReact = 0.0f; g.kprDiveX = 0.0f; g.kprDiveH = KPR_HOME_Y; g.kprLaunched = false; g.kprMove = 0;
    g.scrambleCount = 0; g.scrambleCd = 0.0f;
    g.kprAxis = (Vector3){0, 1, 0};
    g.kprGL = GlovePos(-1); g.kprGR = GlovePos(+1);
    g.kprGLv = (Vector3){0}; g.kprGRv = (Vector3){0};
    g.kprHeadOff = (Vector3){0};
}
static void LaunchDive(void)
{
    Vector3 to = { g.kprDiveX - g.kprPos.x, 0.0f, (GOAL_Z - 0.45f) - g.kprPos.z };
    float d = Vector3Length(to);
    Vector3 dir = (d > 1e-4f) ? Vector3Scale(to, 1.0f/d) : (Vector3){0,0,0};
    if (g.kprMove == 1) {
        // low lateral jolt: drive sideways fast and *down* to the corner — no hop,
        // so the keeper collapses onto a ground-hugging ball instead of leaping over it
        g.kprVel = Vector3Scale(dir, fminf(d / 0.24f, KPR_SHUF_SPEED));
        g.kprVel.y = Clamp((g.kprDiveH - g.kprPos.y) / 0.20f, -3.4f, 0.6f);
    } else {
        g.kprVel = Vector3Scale(dir, fminf(d / 0.32f, KPR_DIVE_SPEED));
        g.kprVel.y = Clamp((g.kprDiveH - g.kprPos.y) / 0.32f + 2.2f, -1.0f, 6.5f);   // spring up into the leap
    }
    g.kprLaunched = true;
}
static void StepKeeper(float dt)
{
    if (!g.kprLaunched) {
        if (g.kprReact > 0.0f) { g.kprReact -= dt; if (g.kprReact <= 0.0f) LaunchDive(); }
        return;
    }
    g.kprVel.y -= 11.0f * dt;                                    // committed: gravity only
    g.kprPos = Vector3Add(g.kprPos, Vector3Scale(g.kprVel, dt));
    if (g.kprPos.y < 0.42f) { g.kprPos.y = 0.42f; g.kprVel.y = 0.0f; g.kprVel.x *= 0.9f; g.kprVel.z *= 0.9f; }
}
// A late lunge for a loose/slow ball near the goal — reuses the same glove/catch logic.
static void TryScramble(void)
{
    if (g.resolved || !g.kprLaunched || g.scrambleCount >= SCRAMBLE_MAX || g.scrambleCd > 0.0f) return;
    float dx = g.ball.pos.x - g.kprPos.x, dz = g.ball.pos.z - g.kprPos.z;
    float dist = sqrtf(dx*dx + dz*dz);
    bool nearGoal = g.ball.pos.z > GOAL_Z - 4.0f && g.ball.pos.z < GOAL_Z + 0.2f;
    if (nearGoal && Vector3Length(g.ball.vel) < 7.0f && dist > 0.5f && dist < SCRAMBLE_REACH) {
        Vector3 dir = { dx/dist, 0.0f, dz/dist };
        g.kprVel = Vector3Scale(dir, fminf(dist / 0.22f, SCRAMBLE_SPEED));
        g.kprVel.y = 2.6f;                                        // hop into the lunge
        g.scrambleCount++; g.scrambleCd = 0.45f;
    }
}
static bool KeeperDeflect(void)
{
    if (g.ball.pos.z < GOAL_Z - 4.0f) return false;
    Vector3 col[3] = { g.kprPos, GlovePos(-1), GlovePos(+1) };
    float    rad[3] = { KPR_BODY, GLOVE_R, GLOVE_R };
    for (int i = 0; i < 3; i++) {
        Vector3 d = Vector3Subtract(g.ball.pos, col[i]);
        float len = Vector3Length(d);
        if (len < BALL_R + rad[i]) {
            Vector3 n = (len > 1e-3f) ? Vector3Scale(d, 1.0f/len) : (Vector3){0, 0, -1};
            float punch = fmaxf(0.0f, Vector3DotProduct(g.kprVel, n));
            Vector3 parry = Vector3Add(Vector3Scale(Vector3Reflect(g.ball.vel, n), 0.5f),
                                       Vector3Scale(n, punch * 0.9f + 2.5f));
            parry.y += 1.5f;
            if (parry.z < -0.6f) { g.ball.vel = parry; g.caught = false; }   // safe deflection
            else                 { g.ball.vel = (Vector3){0}; g.caught = true; g.ball.spin = (Vector3){0}; }
            return true;
        }
    }
    return false;
}

// ---- ragdoll smoothing --------------------------------------------------
static void Spring(Vector3 *pos, Vector3 *vel, Vector3 target, float dt, float k, float d)
{
    Vector3 f = Vector3Subtract(Vector3Scale(Vector3Subtract(target, *pos), k), Vector3Scale(*vel, d));
    *vel = Vector3Add(*vel, Vector3Scale(f, dt));
    *pos = Vector3Add(*pos, Vector3Scale(*vel, dt));
}
static void UpdateGlovesK(float dt, Vector3 tAxis, Vector3 tGL, Vector3 tGR, float k, float d)
{
    g.kprAxis = Vector3Normalize(Vector3Lerp(g.kprAxis, tAxis, Clamp(13.0f*dt, 0.0f, 1.0f)));
    Spring(&g.kprGL, &g.kprGLv, tGL, dt, k, d);
    Spring(&g.kprGR, &g.kprGRv, tGR, dt, k, d);
}
static void UpdateGloves(float dt, Vector3 tAxis, Vector3 tGL, Vector3 tGR)   // soft ragdoll (reactions)
{ UpdateGlovesK(dt, tAxis, tGL, tGR, 130.0f, 15.0f); }
// While the ball is live the hands lead toward it — a committed reach, not a
// trailing follow — so the keeper's arms read as an extension of his attention.
static void ReachForBall(float dt, Vector3 tAxis)
{ UpdateGlovesK(dt, tAxis, GlovePos(-1), GlovePos(+1), 320.0f, 26.0f); }

// ---- keeper reactions ---------------------------------------------------
static void ComputeReactionPose(float t, float dt)
{
    const float standY = 1.05f, groundY = 0.42f;
    g.kprHeadOff = (Vector3){0};

    // dive plays out on the ground before rising (longer for a beaten keeper)
    float onDur   = g.reactCelebrate ? 0.6f : 1.05f;
    float riseDur = g.reactCelebrate ? 0.6f : 0.95f;   // heavier, slower rise out of the dive of despair
    float up;
    if (!g.reactOnGround) up = 1.0f;
    else if (t < onDur)            up = 0.0f;
    else if (t < onDur + riseDur)  up = (t - onDur) / riseDur;
    else                           up = 1.0f;
    up = up*up*(3.0f - 2.0f*up);

    Vector3 lie = (Vector3){ g.reactAxis.x, 0.0f, g.reactAxis.z };     // lie along how it actually landed
    if (Vector3Length(lie) < 0.1f) lie = (Vector3){ (g.reactBase.x >= 0 ? 1.0f : -1.0f), 0.0f, 0.0f };
    lie = Vector3Normalize(lie);
    Vector3 center  = { g.reactBase.x, Lerp(groundY - 0.05f, standY, up), g.reactBase.z };
    Vector3 axis    = Vector3Normalize(Vector3Lerp(lie, (Vector3){0,1,0}, up));
    Vector3 gl = { -0.4f, 0.15f, 0.0f }, gr = { 0.4f, 0.15f, 0.0f };

    float lt = fmaxf(0.0f, t - (g.reactOnGround ? (onDur + riseDur) : 0.0f));
    float w  = lt * 6.0f;
    float gw = t * 7.0f;   // on-ground motion clock

    bool onFloor = g.reactOnGround && up < 0.35f;

    if (onFloor) {
        if (g.reactCelebrate) {                                  // celebrate lying down
            gl = (Vector3){-0.35f, 0.1f + fabsf(sinf(gw))*0.3f, 0.25f};
            gr = (Vector3){ 0.35f, 0.1f + fabsf(sinf(gw+1.5f))*0.3f, 0.25f};
        } else {                                                 // frustration on the ground
            switch (g.reactAnim % 3) {
            case 0: gl = (Vector3){-0.3f, -0.05f - fabsf(sinf(gw*1.6f))*0.15f, 0.4f};   // slam both fists
                    gr = (Vector3){ 0.3f, -0.05f - fabsf(sinf(gw*1.6f))*0.15f, 0.4f}; break;
            case 1: gl = (Vector3){-0.55f, 0.0f, -0.1f};  gr = (Vector3){0.55f, 0.0f, -0.1f}; break;   // arms spread, flat out
            case 2: gl = (Vector3){-0.14f, 0.12f, 0.28f}; gr = (Vector3){0.14f, 0.12f, 0.28f};         // face in hands
                    g.kprHeadOff = (Vector3){ sinf(gw*0.7f)*0.05f, 0, 0 }; break;
            }
        }
    } else if (g.reactCelebrate && g.reactHeld) {
        switch (g.reactAnim) {   // ball rides the RIGHT glove unless dropped
        case 0: gr = (Vector3){0.3f,0.75f,0.0f}; gl.y = 0.15f + fabsf(sinf(w))*0.5f; center.y += fabsf(sinf(w))*0.05f*up; break;
        case 1: gr = (Vector3){0.25f,0.28f,-0.05f}; gl = (Vector3){-0.25f, 0.28f + sinf(w)*0.1f, -0.05f}; center.y += fabsf(sinf(w*1.2f))*0.08f*up; break;
        case 2: if (g.caught) { gr = (Vector3){0.3f,0.05f,0.0f}; gl = (Vector3){-0.35f,0.15f,0.0f}; }
                else { gl.y = 0.15f + fabsf(sinf(w))*0.5f; gr.y = 0.15f + fabsf(sinf(w+1.6f))*0.5f; } break;
        case 3: gr = (Vector3){0.15f,0.55f,0.15f}; gl = (Vector3){-0.35f,0.05f,0.0f}; break;
        case 4: if (g.caught) { gr = (Vector3){0.3f,0.6f,0.0f}; gl = (Vector3){-0.3f,0.4f,0.0f}; }
                else { gl = (Vector3){-0.6f,0.35f,0.0f}; gr = (Vector3){0.6f,0.35f,0.0f}; } break;
        }
    } else if (g.reactCelebrate) {
        switch (g.reactAnim) {
        case 0: gl.y = 0.15f + fabsf(sinf(w))*0.5f; gr.y = 0.15f + fabsf(sinf(w+1.6f))*0.5f; center.y += fabsf(sinf(w))*0.05f*up; break;
        case 1: gl = (Vector3){-0.7f,0.4f,0.0f}; gr = (Vector3){0.7f,0.4f,0.0f}; center.x += sinf(w*0.5f)*0.15f*up;
                axis = Vector3Normalize((Vector3){ sinf(w*0.5f)*0.22f, 1.0f, 0.0f }); break;
        case 2: center.y += fabsf(sinf(w*0.7f))*0.4f*up; gl = (Vector3){-0.3f,0.7f,0.0f}; gr = (Vector3){0.3f,0.7f,0.0f}; break;
        case 3: gr = (Vector3){0.22f,0.78f,0.1f}; gl = (Vector3){-0.35f,0.0f,0.0f}; center.y += fabsf(sinf(w*0.8f))*0.08f*up; break;
        case 4: gl = (Vector3){-0.34f,0.46f,0.12f}; gr = (Vector3){0.34f,0.46f,0.12f}; center.y += fabsf(sinf(w))*0.06f*up; break;
        }
    } else {   // standing frustration
        switch (g.reactAnim) {
        case 0: gl = (Vector3){-0.3f,0.5f,0.1f}; gr = (Vector3){0.3f,0.5f,0.1f}; break;
        case 1: gl = (Vector3){-0.16f,0.6f,0.16f}; gr = (Vector3){0.16f,0.6f,0.16f};
                axis = Vector3Normalize(Vector3Lerp(axis, (Vector3){0,1,-0.4f}, up)); break;
        case 2: center.x += sinf(w*1.6f)*0.08f*up; gr.y = 0.1f + fabsf(sinf(w))*0.25f; break;
        case 3: gl = (Vector3){-0.32f,0.05f,0.0f}; gr = (Vector3){0.32f,0.05f,0.0f}; g.kprHeadOff = (Vector3){ sinf(w)*0.12f, 0.0f, 0.0f }; break;
        case 4: gl = (Vector3){-0.45f,-0.2f,0.0f}; gr = (Vector3){0.45f,-0.2f,0.0f}; center.x += sinf(w*0.35f)*0.12f*up; break;
        }
    }

    g.kprPos = Vector3Lerp(g.kprPos, center, Clamp(14.0f*dt, 0.0f, 1.0f));
    UpdateGloves(dt, axis, Vector3Add(center, gl), Vector3Add(center, gr));
}
static void KeeperReact(float dt)
{
    if (!g.landed) {                          // let the dive finish — fall to the turf
        g.kprVel.y -= 11.0f * dt;
        g.kprPos = Vector3Add(g.kprPos, Vector3Scale(g.kprVel, dt));
        float sp = Vector3Length(g.kprVel);
        Vector3 tA = (g.kprLaunched && sp > 1.0f) ? Vector3Scale(g.kprVel, 1.0f/sp) : (Vector3){0,1,0};
        ReachForBall(dt, tA);                                 // stay reaching through the parry
        if (g.kprPos.y <= 0.42f) { g.kprPos.y = 0.42f; g.landed = true; g.reactT = 0.0f; g.reactBase = g.kprPos; g.reactAxis = g.kprAxis; }
    } else {
        g.reactT += dt;
        ComputeReactionPose(g.reactT, dt);
        if (g.reactHeld && g.caught &&
            ((g.reactAnim == 2 && g.reactT > 1.2f) || (g.reactAnim == 4 && g.reactT > 1.0f))) {
            g.caught = false;
            if (g.reactAnim == 4) g.ball.vel = (Vector3){ 0.0f, -1.5f, -2.5f };
        }
    }
}

// ---- flow ---------------------------------------------------------------
static void StartKick(void)
{
    g.phase = PHASE_AIM;
    g.yaw = 0.0f; g.pitch = 0.16f; g.curve = 0.0f; g.power01 = 0.0f; g.chargeDir = 1.0f;
    g.ball = (Ball){0}; g.ball.pos = (Vector3){0.0f, BALL_R, 0.0f};
    ResetKeeper(); InitNet();
    g.result = RES_NONE; g.resolved = false; g.caught = false; g.hitWood = false;
    g.resultTimer = 0.0f; g.flight = 0.0f; g.landed = false; g.reactT = 0.0f; g.touchCharge = false;
    g.cine = false; g.cineArmed = false; g.closest = 1e9f; gBallRot = QuaternionIdentity();
}
static void SetupRound(int r)
{
    Moment m = gRounds[r];
    gEra = m.era; gHome = TEAMS[m.home]; gAway = TEAMS[m.away];
    gFaceSel = (m.home + r*2 + 1) % FACE_SKINS;          // keeper's look varies by round
    g.target = m.oppGoals;                               // beat the opponent's tally
    for (int i = 0; i < KICKS_TOTAL; i++) g.kickRes[i] = RES_NONE;
    g.kick = 0; g.scored = 0;
    StartKick();
}
static void ResetMatch(void)
{
    // draw ROUNDS distinct moments at random (Fisher-Yates), then order them by
    // the opponent's tally -> ascending difficulty. Different every session.
    int n = (int)(sizeof(POOL)/sizeof(POOL[0]));
    int idx[32];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = n - 1; i > 0; i--) { int j = GetRandomValue(0, i); int s = idx[i]; idx[i] = idx[j]; idx[j] = s; }
    Moment t[ROUNDS];
    for (int i = 0; i < ROUNDS; i++) t[i] = POOL[idx[i]];
    for (int i = 0; i < ROUNDS; i++) for (int j = i+1; j < ROUNDS; j++)
        if (t[j].oppGoals < t[i].oppGoals) { Moment s = t[i]; t[i] = t[j]; t[j] = s; }
    for (int i = 0; i < ROUNDS; i++) gRounds[i] = t[i];
    g.round = 0; g.roundWon = false; g.gameWon = false; g.gameLost = false; g.celebrate = 0.0f;
    SetupRound(0);
}

static Strike AimStrike(void)
{
    float scatter = g.power01 * 0.05f;
    return (Strike){ .power = 13.0f + g.power01 * 16.0f,
                     .yaw = g.yaw + Frand2()*scatter, .pitch = g.pitch + Frand2()*scatter*0.6f,
                     .curve = g.curve * 3.0f };
}
static void CommitKeeperDive(void)
{
    Ball p = g.ball;
    for (int i = 0; i < 500 && p.pos.z < GOAL_Z; i++) BallStep(&p, 0.01f);
    float px = p.pos.x, py = p.pos.y;

    // Low-and-wide balls hug the ground into a corner — the leaping dive hops over
    // them. For those, pick the low lateral shuffle: it reads quicker, commits
    // closer to the real corner, and stays down. (Still fallible: err + the
    // wrong-way guess remain, so it's a fighting chance, not a wall.)
    bool lowWide = (py < 0.85f) && (fabsf(px) > 1.6f);
    g.kprMove = lowWide ? 1 : 0;

    float err = Frand2() * (lowWide ? 1.15f : 1.9f);              // reads the low roller a touch better
    if (Frand() < (lowWide ? 0.12f : 0.18f)) err += (px > 0 ? -1.0f : 1.0f) * 2.5f;   // still guesses wrong sometimes
    float reachX = lowWide ? 0.94f : 0.8f;                        // shuffle commits nearer the post
    g.kprDiveX = Clamp(px * reachX + err, -GOAL_HALF_W - 0.6f, GOAL_HALF_W + 0.6f);
    g.kprDiveH = lowWide ? Clamp(py * 0.9f + 0.12f, 0.25f, 0.95f)
                         : Clamp(py * 0.85f + 0.35f, 0.5f, GOAL_H);
    g.kprReact = (lowWide ? 0.12f : 0.16f) + Frand() * 0.11f;
    g.kprLaunched = false; g.kprVel = (Vector3){0};

    // Arm a bullet-time replay from what we can already predict: how near the
    // keeper's dive lands to the ball's line, and whether it rattles the post.
    // Deciding here (not at resolution) lets the slow-mo catch the *lead-up*.
    float gap     = sqrtf((px-g.kprDiveX)*(px-g.kprDiveX) + (py-g.kprDiveH)*(py-g.kprDiveH));
    float postGap = fabsf(fabsf(px) - GOAL_HALF_W);
    bool inFrame  = fabsf(px) < GOAL_HALF_W + 0.5f && py < GOAL_H + 0.4f;
    float pc = 0.05f;
    if      (inFrame && gap < 0.40f) pc = 0.80f;         // fingertip drama
    else if (inFrame && gap < 0.75f) pc = 0.42f;
    else if (inFrame && gap < 1.15f) pc = 0.18f;
    if (postGap < 0.28f)             pc = fmaxf(pc, 0.72f);   // off the woodwork
    g.cineArmed = inFrame && (Frand() < pc);
    if (g.cineArmed) {                                   // pre-frame the point of contact
        g.cineFocus = (Vector3){ (px + g.kprDiveX)*0.5f, Clamp((py + g.kprDiveH)*0.5f, 0.6f, 2.0f), GOAL_Z - 0.35f };
        g.cineAng0  = (px >= 0.0f) ? -0.45f : 0.45f;
        g.cineDir   = (px >= 0.0f) ?  1.0f : -1.0f;
    }
}
static void Resolve(Result r)
{
    g.result = r; g.resolved = true; g.resultTimer = 0.0f;
    if (g.kick < KICKS_TOTAL) g.kickRes[g.kick] = r;
    if (r == RES_GOAL) g.scored++;
    g.celebrate = 1.7f;                 // crowd-reaction window (each stand reacts by outcome)
    g.reactAnim = GetRandomValue(0, 4);
    g.reactCelebrate = (r != RES_GOAL);
    g.reactHeld = (r == RES_SAVE && g.caught);
    g.reactT = 0.0f; g.kprHeadOff = (Vector3){0};
    if (!g.kprLaunched || (g.kprPos.y > 0.85f && fabsf(g.kprVel.y) < 2.0f)) {
        g.landed = true;  g.reactOnGround = false; g.reactBase = (Vector3){ g.kprPos.x, 1.05f, g.kprPos.z }; g.reactAxis = (Vector3){0,1,0};
    } else { g.landed = false; g.reactOnGround = true; }
    // NOTE: the replay is armed in CommitKeeperDive and engaged pre-contact, so
    // the slow-mo captures the approach and the moment of impact. Once contact
    // lands, wind the replay down quickly so it lingers on the moment, not after.
    if (g.cine) g.cineDur = fminf(g.cineDur, g.cineT + 0.95f);
}

// turf: bounce if descending fast, else roll — and KILL spin so it can't spiral
static void Ground(float dt, int N)
{
    if (g.ball.pos.y > BALL_R) return;
    g.ball.pos.y = BALL_R;
    if (g.ball.vel.y < -1.6f) { g.ball.vel.y *= -0.45f; g.ball.vel.x *= 0.9f; g.ball.vel.z *= 0.9f; }
    else {
        g.ball.vel.y = 0.0f;
        float roll = 1.0f - 1.4f * (dt / N);                   // real rolling friction
        g.ball.vel.x *= roll; g.ball.vel.z *= roll;
        g.ball.spin = (Vector3){0};                            // grounded ball doesn't Magnus-curl
        if (Vector3Length(g.ball.vel) < 0.35f) g.ball.vel = (Vector3){0};
    }
}
static void Bounds(void)   // stadium walls so nothing sails into the fans
{
    if (g.ball.pos.z > BACK_WALL)  { g.ball.pos.z = BACK_WALL; g.ball.vel.z *= -0.3f; g.ball.vel.x *= 0.7f; }
    if (fabsf(g.ball.pos.x) > 16.0f) { g.ball.pos.x = (g.ball.pos.x>0?16.0f:-16.0f); g.ball.vel.x *= -0.3f; }
}

static void StepBallLive(float dt)
{
    const int N = 8;
    for (int i = 0; i < N; i++) {
        Vector3 prev = g.ball.pos;
        BallStep(&g.ball, dt / N);
        Vector3 p = g.ball.pos;
        // how close did it come to keeper/gloves? (drives the replay bias)
        if (p.z > GOAL_Z - 5.0f) {
            Vector3 cp[3] = { g.kprPos, GlovePos(-1), GlovePos(+1) };
            float   cr[3] = { KPR_BODY, GLOVE_R, GLOVE_R };
            for (int k = 0; k < 3; k++) {
                float d = Vector3Length(Vector3Subtract(p, cp[k])) - cr[k];
                if (d < g.closest) g.closest = d;
            }
        }
        if (KeeperDeflect()) { Resolve(RES_SAVE); return; }
        if (prev.z < GOAL_Z && p.z >= GOAL_Z) {
            float t  = (GOAL_Z - prev.z) / (p.z - prev.z);
            float xc = prev.x + (p.x - prev.x) * t, yc = prev.y + (p.y - prev.y) * t;
            bool onPost = fabsf(fabsf(xc) - GOAL_HALF_W) < BALL_R + POST_R && yc > 0.0f && yc < GOAL_H + 0.15f;
            bool onBar  = fabsf(yc - GOAL_H) < BALL_R + POST_R && fabsf(xc) < GOAL_HALF_W + 0.15f;
            if (onPost || onBar) {
                Vector3 axisPt = onBar ? (Vector3){ p.x, GOAL_H, GOAL_Z }
                                       : (Vector3){ (xc > 0 ? GOAL_HALF_W : -GOAL_HALF_W), p.y, GOAL_Z };
                Vector3 nrm = Vector3Subtract(p, axisPt);
                nrm = (Vector3Length(nrm) > 1e-4f) ? Vector3Normalize(nrm) : (Vector3){0,0,-1};
                g.ball.vel = Vector3Scale(Vector3Reflect(g.ball.vel, nrm), 0.6f);
                g.ball.pos.z = GOAL_Z - BALL_R; g.hitWood = true;
            } else {
                bool in = fabsf(xc) < GOAL_HALF_W && yc > 0.0f && yc < GOAL_H;
                Resolve(in ? RES_GOAL : (g.hitWood ? RES_POST : RES_MISS)); return;
            }
        }
        Ground(dt, N);
    }
    if (g.ball.pos.z < GOAL_Z - 0.3f && g.ball.pos.y <= BALL_R + 0.05f && Vector3Length(g.ball.vel) < 0.4f)
        Resolve(g.hitWood ? RES_POST : RES_MISS);
}
static void StepBallSettle(float dt)
{
    if (g.caught) {
        g.ball.pos = Vector3Lerp(g.ball.pos, g.kprGR, Clamp(9.0f * dt, 0.0f, 1.0f));
        g.ball.vel = (Vector3){0};
        return;
    }
    const int N = 6;
    float backZ = GOAL_Z + NET_DEPTH;
    for (int i = 0; i < N; i++) {
        BallStep(&g.ball, dt / N);
        if (g.ball.pos.z > GOAL_Z && fabsf(g.ball.pos.x) < GOAL_HALF_W && g.ball.pos.y < GOAL_H) {
            g.ball.vel = Vector3Scale(g.ball.vel, 1.0f - 4.5f*(dt/N));
            if (g.ball.pos.z > backZ + 0.35f) { g.ball.pos.z = backZ + 0.35f; if (g.ball.vel.z > 0) g.ball.vel.z *= -0.12f; }
            if (g.ball.pos.x >  GOAL_HALF_W - BALL_R) { g.ball.pos.x =  GOAL_HALF_W - BALL_R; g.ball.vel.x *= -0.2f; }
            if (g.ball.pos.x < -GOAL_HALF_W + BALL_R) { g.ball.pos.x = -GOAL_HALF_W + BALL_R; g.ball.vel.x *= -0.2f; }
            if (g.ball.pos.y >  GOAL_H - BALL_R)      { g.ball.pos.y =  GOAL_H - BALL_R;      g.ball.vel.y *= -0.2f; }
        }
        Ground(dt, N);
        Bounds();
    }
}

// ---- stadium (bleachers + fans) -----------------------------------------
// colorblocked stands: left half wears the home kit, right half the away kit
static Color FanColor(float x, int h)
{
    Team tm = (x < 0.0f) ? gHome : gAway;
    int m = h % 5;
    if (m == 0) return (Color){235,235,235,255};
    return (m <= 2) ? tm.a : tm.b;
}
static void DrawBoardOverlay(void)   // project the board rect + render the live match in an era style
{
    int W = GetScreenWidth(), H = GetScreenHeight();
    float bw = fminf(W * 0.64f, 640.0f);
    Rectangle rect = { (W - bw) * 0.5f, H * 0.03f, bw, bw / 3.3f };   // fixed top-center panel

    BoardData bd = (BoardData){0};
    bd.era = gEra; bd.year = gRounds[g.round].year;
    bd.title = TextFormat("ROUND %d / %d", g.round+1, ROUNDS);
    bd.sub   = TextFormat("BEAT %d", g.target);
    if (g.phase == PHASE_RESULT) {
        if (g.gameWon)       { bd.title = "CHAMPIONS"; bd.sub = "you win the cup"; bd.big = "WINNERS"; bd.bigColor = (Color){250,215,70,255}; }
        else if (g.gameLost) { bd.title = "GAME OVER"; bd.sub = TextFormat("scored %d, needed %d", g.scored, g.target+1); bd.big = "OUT"; bd.bigColor = (Color){235,90,90,255}; }
        else                 { bd.title = "ROUND CLEARED"; bd.sub = TextFormat("scored %d", g.scored); bd.big = "ADVANCE"; bd.bigColor = (Color){90,220,120,255}; }
    } else if (g.phase == PHASE_FLY && g.resolved) {
        bd.big = g.result==RES_GOAL?"GOAL!":g.result==RES_SAVE?"SAVED!":g.result==RES_POST?"WOODWORK!":"MISS!";
        bd.bigColor = g.result==RES_GOAL ? (Color){90,220,120,255} : (Color){235,90,90,255};
    } else { bd.big = TextFormat("YOU  %d", g.scored); bd.bigColor = RAYWHITE; }
    bd.homeA = gHome.a; bd.homeB = gHome.b; bd.awayA = gAway.a; bd.awayB = gAway.b;
    bd.dots = KICKS_TOTAL;
    for (int i = 0; i < KICKS_TOTAL; i++) {
        bool played = i < g.kick || (i == g.kick && g.resolved);
        Result rr = (i < g.kick) ? g.kickRes[i] : g.result;
        bd.dotState[i] = played ? (rr==RES_GOAL ? 1 : 2) : 0;
    }
    DrawJumbotron(rect, bd, GetTime());
}
static const Color CROWD_SKIN[3] = { {224,178,138,255}, {188,140,100,255}, {130,90,64,255} };

// one spectator: a colored torso cube + a small head, bobbing by `bob`
static void DrawFan(float x, float y, float z, float bob, Color body, int h)
{
    DrawCube((Vector3){x, y + 0.35f + bob, z}, 0.26f, 0.40f, 0.26f, body);
    DrawCube((Vector3){x, y + 0.63f + bob, z}, 0.17f, 0.17f, 0.17f, CROWD_SKIN[h % 3]);
}
// a waving flag on a pole (drawn both windings so it reads from any angle)
static void DrawFlag(Vector3 base, float t, float ph, Color col)
{
    Vector3 top = { base.x, base.y + 2.0f, base.z };
    DrawCylinderEx(base, top, 0.04f, 0.04f, 6, (Color){60,60,66,255});
    const int N = 6; const float W = 1.25f, FH = 0.72f;
    for (int i = 0; i < N; i++) {
        float u0 = (float)i/N, u1 = (float)(i+1)/N;
        float w0 = sinf(t*4.0f + ph + u0*6.0f) * 0.20f * u0;
        float w1 = sinf(t*4.0f + ph + u1*6.0f) * 0.20f * u1;
        Vector3 a = { base.x + u0*W, top.y,       base.z + w0 };
        Vector3 b = { base.x + u1*W, top.y,       base.z + w1 };
        Vector3 c = { base.x + u1*W, top.y - FH,  base.z + w1 };
        Vector3 d = { base.x + u0*W, top.y - FH,  base.z + w0 };
        DrawTriangle3D(a, c, b, col); DrawTriangle3D(a, d, c, col);
        DrawTriangle3D(a, b, c, col); DrawTriangle3D(a, c, d, col);
    }
}
// pop-off camera flashes scattered through the stands (a storm of them on a goal)
static void DrawFlashes(float t, int n, float rate)
{
    for (int i = 0; i < n; i++) {
        float ph = i * 127.13f;
        if (sinf(t*rate + ph*1.7f) < 0.86f) continue;
        float fx = -16.0f + fmodf(ph*7.31f, 32.0f);
        float fz = GOAL_Z + 2.4f + fmodf(ph*2.11f, 6.6f);
        float fy = 0.7f  + fmodf(ph*1.73f, 4.2f);
        DrawCube((Vector3){fx, fy, fz}, 0.14f, 0.14f, 0.14f, (Color){255,255,246,255});
    }
}
static void DrawFloodlight(float x, float z)
{
    Vector3 base = { x, 0.0f, z }, top = { x, 8.6f, z };
    DrawCylinderEx(base, top, 0.22f, 0.16f, 8, (Color){44,46,54,255});
    Vector3 head = { x, 9.1f, z };
    DrawCubeV(head, (Vector3){2.4f, 0.7f, 0.35f}, (Color){60,62,72,255});
    for (int i = -1; i <= 1; i++)                                   // lamp cells
        DrawCubeV((Vector3){x + i*0.75f, 9.1f, z - 0.2f}, (Vector3){0.55f, 0.5f, 0.12f}, (Color){255,250,225,255});
    DrawSphere((Vector3){x, 9.1f, z - 0.35f}, 0.9f, (Color){255,250,220,26});   // soft glow
}
// pitch-side sponsor hoardings, a row behind the goal facing the pitch
static void DrawBanners(void)
{
    const int n = NSPON;
    float bh = 0.9f, y = 0.72f, z = GOAL_Z + 2.15f, gap = 0.22f;
    float w[NSPON], total = 0.0f;                        // each board as wide as its (variable) texture
    for (int i = 0; i < n; i++) {
        Texture2D t = gBannerTex[(i + g.round) % NSPON];
        w[i] = bh * ((float)t.width / (float)t.height);
        total += w[i] + (i ? gap : 0.0f);
    }
    DrawCube((Vector3){0, y, z + 0.06f}, total + 0.5f, bh + 0.12f, 0.1f, (Color){18,20,26,255});   // backing rail
    float x = -total/2.0f;
    for (int i = 0; i < n; i++) {
        Texture2D t = gBannerTex[(i + g.round) % NSPON];
        DrawDecal(t, (Vector3){x + w[i]/2.0f, y, z}, (Vector3){0,0,-1}, (Vector3){0,1,0}, w[i], bh);
        x += w[i] + gap;
    }
}
static void DrawStadium(void)
{
    float t = GetTime();
    bool react = g.celebrate > 0.0f;
    bool kickerScored = (g.result == RES_GOAL);       // kicker's fans sit in the away (right) stand

    // dark stadium bowl behind the stands, to frame the crowd
    DrawCubeV((Vector3){0, 4.0f, GOAL_Z + 12.5f}, (Vector3){44.0f, 9.0f, 1.2f}, (Color){22,24,34,255});
    DrawFloodlight(-19.5f, GOAL_Z + 1.0f);  DrawFloodlight(19.5f, GOAL_Z + 1.0f);
    DrawFloodlight(-19.5f, GOAL_Z + 10.0f); DrawFloodlight(19.5f, GOAL_Z + 10.0f);

    // ---- back stand (behind the goal) ----
    const int ROWS = 8, COLS = 54;
    for (int r = 0; r < ROWS; r++) {
        float z = GOAL_Z + 2.7f + r*1.15f;
        float y = 0.2f + r*0.8f;
        DrawCube((Vector3){0, y - 0.5f, z}, 37.0f, 1.0f, 1.15f, (Color){ 52,54,64,255 });
        DrawCube((Vector3){0, y - 0.02f, z - 0.5f}, 37.0f, 0.09f, 0.12f, (Color){ 36,38,46,255 });
        for (int c = 0; c < COLS; c++) {
            int h = (r*37 + c*19) % 100;
            float x = -17.0f + c*(34.0f/(COLS-1));
            float yoff = fmaxf(0.0f, sinf(t*1.3f - x*0.22f))*0.10f + sinf(t*2.0f + h)*0.03f;
            if (react) {
                bool home = (x < 0.0f);                                  // home = the keeper's stand
                bool happy = home ? !kickerScored : kickerScored;        // each stand reacts to the outcome
                if (happy) yoff = fabsf(sinf(t*12.0f + h)) * 0.36f;       // leaping to their feet
                else       yoff = -0.14f + sinf(t*2.5f + h)*0.02f;        // slumped, seated
            }
            DrawFan(x, y, z, yoff, FanColor(x, h), h);
        }
    }
    // ---- side stands (along the touchlines) ----
    const int SROWS = 5, SCOLS = 22;
    for (int side = -1; side <= 1; side += 2) {
        for (int r = 0; r < SROWS; r++) {
            float x = side * (18.6f + r*0.95f);
            float y = 0.3f + r*0.8f;
            DrawCube((Vector3){x, y - 0.5f, GOAL_Z + 2.5f}, 1.3f, 1.0f, 15.0f, (Color){ 48,50,60,255 });
            for (int c = 0; c < SCOLS; c++) {
                int h = (r*29 + c*23 + side*7) % 100;
                float z = GOAL_Z - 3.4f + c*(13.0f/(SCOLS-1));
                float yoff = fmaxf(0.0f, sinf(t*1.1f + z*0.3f))*0.09f + sinf(t*1.8f + h)*0.03f;
                if (react) {
                    bool happy = (x < 0.0f) ? !kickerScored : kickerScored;
                    yoff = happy ? fabsf(sinf(t*11.0f + h))*0.32f : -0.12f;
                }
                DrawFan(x, y, z, yoff, FanColor(x, h), h);
            }
        }
    }
    // ---- banners waving over the back stand ----
    for (int i = 0; i < 6; i++) {
        float fx = -14.0f + i*5.4f;
        Color fc = (i % 2) ? gAway.a : gHome.a;
        DrawFlag((Vector3){fx, 5.6f, GOAL_Z + 3.2f}, t, i*1.7f, fc);
    }
    // ---- camera flashes: a constant sprinkle, a storm on a goal/save ----
    DrawFlashes(t, react ? 90 : 16, react ? 9.0f : 2.4f);
}

// ---- pitch markings -----------------------------------------------------
static void PitchLineH(float z, float x0, float x1)
{ DrawCube((Vector3){(x0+x1)*0.5f, 0.02f, z}, fabsf(x1-x0), 0.03f, 0.12f, (Color){235,238,240,200}); }
static void PitchLineV(float x, float z0, float z1)
{ DrawCube((Vector3){x, 0.02f, (z0+z1)*0.5f}, 0.12f, 0.03f, fabsf(z1-z0), (Color){235,238,240,200}); }
static void DrawPitch(void)
{
    const float PW = 20.15f, GW = 9.16f;
    PitchLineH(GOAL_Z, -PW, PW);
    PitchLineH(-5.5f,  -PW, PW); PitchLineV(-PW, -5.5f, GOAL_Z); PitchLineV(PW, -5.5f, GOAL_Z);
    PitchLineH(5.5f, -GW, GW);   PitchLineV(-GW, 5.5f, GOAL_Z);  PitchLineV(GW, 5.5f, GOAL_Z);
    DrawCube((Vector3){0, 0.02f, 0}, 0.22f, 0.03f, 0.22f, (Color){235,238,240,220});
}

// ---- input --------------------------------------------------------------
static void AimFromPress(Vector2 m)   // X-Y position selects the trajectory
{
    float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    g.yaw   = Clamp((m.x/W - 0.5f) * 2.0f * 0.42f, -0.42f, 0.42f);
    g.pitch = Clamp(0.05f + (1.0f - m.y/H) * 0.5f, 0.04f, 0.52f);
}
static void FireShot(void)
{
    g.ball = BallLaunch((Vector3){0, BALL_R, 0}, AimStrike());
    CommitKeeperDive();
    g.phase = PHASE_FLY;
}

// ---- keeper model (Mii-ish: head + rounded body + arms + two-segment legs) ----
static void DrawKeeper(void)
{
    Vector3 up = (Vector3Length(g.kprAxis) > 0.1f) ? Vector3Normalize(g.kprAxis) : (Vector3){0,1,0};
    Vector3 side = Vector3CrossProduct(up, (Vector3){0,0,1});
    if (Vector3Length(side) < 0.1f) side = (Vector3){1,0,0};
    side = Vector3Normalize(side);
    Vector3 fwd = Vector3CrossProduct(up, side);                 // body front (~ -z upright)
    if (Vector3Length(fwd) < 0.1f) fwd = (Vector3){0,0,-1};
    fwd = Vector3Normalize(fwd);

    Vector3 hip      = Vector3Add(g.kprPos, Vector3Scale(up, -0.30f));
    Vector3 shoulder = Vector3Add(g.kprPos, Vector3Scale(up,  0.24f));
    Vector3 headPos  = Vector3Add(Vector3Add(g.kprPos, Vector3Scale(up, 0.74f)), g.kprHeadOff);  // raised so the chin clears the collar

    Color kit  = gHome.a;
    Color sock = gHome.b;
    if (g.resolved && !g.reactCelebrate) {                        // beaten keeper darkens
        kit  = (Color){ (unsigned char)(kit.r*0.7f),  (unsigned char)(kit.g*0.7f),  (unsigned char)(kit.b*0.7f),  255 };
        sock = (Color){ (unsigned char)(sock.r*0.7f), (unsigned char)(sock.g*0.7f), (unsigned char)(sock.b*0.7f), 255 };
    }
    Color boot = (Color){ 28, 28, 34, 255 };

    // ---- leg animation params by stance (nothing is ever fully static) ----
    float t = GetTime();
    bool  standing = up.y > 0.82f;
    float crouch = 0.0f, spread = 0.15f, kneeF = 0.10f, bounce = 0.0f, kick = 0.0f;
    if (g.phase == PHASE_CHARGE || (g.phase == PHASE_AIM)) {      // set, ready to spring
        float c = (g.phase == PHASE_CHARGE) ? g.power01 : 0.15f;
        crouch = 0.05f + c*0.13f; spread = 0.17f + c*0.03f; kneeF = 0.14f + c*0.06f;
        bounce = sinf(t*2.4f)*0.012f;
    } else if (!standing) {                                       // airborne / diving: scissor the legs
        kick = 0.16f; spread = 0.19f;
    } else {                                                      // idle breathing sway
        bounce = sinf(t*2.0f)*0.015f; crouch = 0.02f;
        if (g.resolved && g.reactCelebrate) kick = 0.10f + fabsf(sinf(t*7.0f))*0.06f;   // happy jig
    }
    Vector3 hipBase = Vector3Add(hip, Vector3Scale(up, -crouch + bounce));

    // legs: hip -> knee (thigh, kit shorts) -> foot (shin, sock) -> boot
    for (int s = -1; s <= 1; s += 2) {
        float ph = (s < 0) ? 0.0f : PI;                          // opposite phase per leg
        float kb = kneeF + kick*0.5f*sinf(t*9.0f + ph);          // knee drive
        float lift = kick * fmaxf(0.0f, sinf(t*9.0f + ph)) * 0.14f;
        Vector3 hp  = Vector3Add(hipBase, Vector3Scale(side, s*spread));
        Vector3 kn  = Vector3Add(Vector3Add(hp, Vector3Scale(up, -0.28f + lift)),
                                 Vector3Add(Vector3Scale(fwd, kb), Vector3Scale(side, s*0.02f)));
        Vector3 ft  = Vector3Add(Vector3Add(kn, Vector3Scale(up, -0.26f + lift*0.5f)),
                                 Vector3Scale(fwd, 0.06f + kick*0.5f*sinf(t*9.0f + ph)));
        Vector3 toe = Vector3Add(Vector3Add(ft, Vector3Scale(fwd, 0.15f)), Vector3Scale(up, -0.03f));
        DrawCylinderEx(hp, kn, 0.12f, 0.10f, 10, kit);           // thigh (shorts)
        DrawSphere(kn, 0.10f, sock);                             // knee
        DrawCylinderEx(kn, ft, 0.10f, 0.075f, 10, sock);         // shin (sock)
        DrawCylinderEx(ft, toe, 0.085f, 0.05f, 8, boot);         // boot
        DrawSphere(ft, 0.085f, boot);                            // heel
    }

    DrawCapsule(hip, shoulder, 0.30f, 12, 10, kit);              // rounded torso
    Vector3 shL = KprShoulder(-1), shR = KprShoulder(+1);        // arms hinge where the reach originates
    DrawCylinderEx(shL, g.kprGL, 0.09f, 0.06f, 8, kit);          // upper arms -> gloves
    DrawCylinderEx(shR, g.kprGR, 0.09f, 0.06f, 8, kit);
    DrawSphere(g.kprGL, GLOVE_R, gHome.b);                       // gloves (hands)
    DrawSphere(g.kprGR, GLOVE_R, gHome.b);

    // neck: a short skin column bridging collar to head, so a chin reads
    Color neck = (Color){ (unsigned char)(gSkinTone[gFaceSel].r*0.86f), (unsigned char)(gSkinTone[gFaceSel].g*0.86f),
                          (unsigned char)(gSkinTone[gFaceSel].b*0.86f), 255 };
    DrawCylinderEx(Vector3Add(g.kprPos, Vector3Scale(up, 0.34f)),
                   Vector3Add(headPos, Vector3Scale(up, -0.22f)), 0.135f, 0.12f, 10, neck);

    // ---- head: shaded skin sphere, its +Z pole (hair cap) aimed at "up" ----
    gHeadModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = gHeadTex[gFaceSel];
    gHeadModel.transform = QuaternionToMatrix(QuaternionFromVector3ToVector3((Vector3){0,0,1}, up));
    DrawModel(gHeadModel, headPos, 1.0f, WHITE);

    // ---- face: an expression decal, presented mostly to the front but turning
    // a touch toward the ball so the keeper reads as watching it ----
    int expr = EXPR_FOCUS;
    if (g.resolved)            expr = g.reactCelebrate ? EXPR_HAPPY : EXPR_ANGRY;
    else if (Vector3Length(Vector3Subtract(g.ball.pos, headPos)) < 1.7f) expr = EXPR_WORRY;
    Vector3 gaze = (!g.resolved) ? Vector3Subtract(g.ball.pos, headPos) : fwd;
    if (Vector3Length(gaze) < 1e-3f) gaze = fwd;
    gaze = Vector3Normalize(gaze);
    Vector3 look = Vector3Normalize(Vector3Lerp(fwd, gaze, 0.35f));   // subtle turn, stays front-on
    Vector3 facePos = Vector3Add(headPos, Vector3Scale(look, 0.30f));
    DrawDecal(gExprTex[expr], facePos, look, up, 0.58f, 0.58f);
}

// ---- replay camera ------------------------------------------------------
// Eases the camera between its fixed match view and an orbiting closeup that
// spins around the save/goal during a bullet-time replay.
static void DriveCamera(float dt)
{
    Vector3 wantP = CAM_POS, wantT = CAM_TGT;
    if (g.cine) {
        float u  = Clamp(g.cineT / g.cineDur, 0.0f, 1.0f);
        float R  = Lerp(3.3f, 2.1f, u);                       // tight, slowly dollying closeup
        float th = g.cineAng0 + g.cineDir * u * 2.4f;         // ~140° arc, lingering on contact
        Vector3 f = g.cineFocus;
        wantT = Vector3Lerp(f, g.ball.pos, 0.35f);            // bias framing onto the ball
        wantP = (Vector3){ f.x + sinf(th)*R, f.y + 0.75f + sinf(u*PI)*0.4f, f.z - cosf(th)*R };
    }
    float k = Clamp((g.cine ? 10.0f : 4.5f) * dt, 0.0f, 1.0f);
    g.cam.position = Vector3Lerp(g.cam.position, wantP, k);
    g.cam.target   = Vector3Lerp(g.cam.target,   wantT, k);
}

// ---- main frame ---------------------------------------------------------
static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;
    if (g.celebrate > 0.0f) g.celebrate -= dt;
    if (g.scrambleCd > 0.0f) g.scrambleCd -= dt;
    // engage the armed replay as the ball enters its final approach, so the
    // slow-mo runs through the lead-up and the moment of contact (not just after)
    if (g.phase == PHASE_FLY && !g.resolved && g.cineArmed && !g.cine && g.ball.pos.z > GOAL_Z - 3.0f) {
        g.cine = true; g.cineArmed = false; g.cineT = 0.0f; g.cineDur = 3.6f;
    }
    if (g.cine) { g.cineT += dt; if (g.cineT >= g.cineDur) g.cine = false; }
    float sdt = g.cine ? dt * 0.13f : dt;                     // bullet-time slows the sim hard
    Vector2 m = GetMousePosition();

    switch (g.phase) {
    case PHASE_AIM: {
        float s = 0.9f * dt;                                    // keyboard aim (desktop)
        if (IsKeyDown(KEY_LEFT))  g.yaw   -= s;
        if (IsKeyDown(KEY_RIGHT)) g.yaw   += s;
        if (IsKeyDown(KEY_UP))    g.pitch += s;
        if (IsKeyDown(KEY_DOWN))  g.pitch -= s;
        g.yaw = Clamp(g.yaw, -0.42f, 0.42f); g.pitch = Clamp(g.pitch, 0.02f, 0.55f);
        if (IsKeyDown(KEY_A)) g.curve = Clamp(g.curve - 1.4f*dt, -1.0f, 1.0f);
        if (IsKeyDown(KEY_D)) g.curve = Clamp(g.curve + 1.4f*dt, -1.0f, 1.0f);
        // touch/mouse: press position sets trajectory, then hold to charge
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            AimFromPress(m); g.pressX = m.x; g.curve = 0.0f;
            g.touchCharge = true; g.power01 = 0.0f; g.chargeDir = 1.0f; g.phase = PHASE_CHARGE;
        }
        if (IsKeyPressed(KEY_SPACE)) { g.touchCharge = false; g.power01 = 0.0f; g.chargeDir = 1.0f; g.phase = PHASE_CHARGE; }
    } break;

    case PHASE_CHARGE: {
        g.power01 += g.chargeDir * 1.6f * dt;                   // hold charges & uncharges
        if (g.power01 >= 1.0f) { g.power01 = 1.0f; g.chargeDir = -1.0f; }
        if (g.power01 <= 0.0f) { g.power01 = 0.0f; g.chargeDir =  1.0f; }
        if (g.touchCharge) {
            float W = (float)GetScreenWidth();
            g.curve = Clamp((m.x - g.pressX) / (W * 0.14f), -1.0f, 1.0f);   // drag = spin
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) FireShot();
        } else {
            if (IsKeyReleased(KEY_SPACE) || IsKeyPressed(KEY_SPACE)) FireShot();
        }
    } break;

    case PHASE_FLY: {
        StepNet(sdt);
        if (!g.resolved) {
            StepKeeper(sdt);
            TryScramble();                     // late lunge for a loose/slow ball
            { float sp = Vector3Length(g.kprVel);
              Vector3 tA = (g.kprLaunched && sp > 1.0f) ? Vector3Scale(g.kprVel, 1.0f/sp) : (Vector3){0,1,0};
              ReachForBall(sdt, tA); }         // hands lead toward the ball
            StepBallLive(sdt);
            SpinBall(sdt);
            g.flight += dt;
            if (!g.cine && g.flight > 5.0f) Resolve(g.hitWood ? RES_POST : RES_MISS);
        } else {
            KeeperReact(sdt); StepBallSettle(sdt); SpinBall(sdt); g.resultTimer += sdt;
            if (!g.cine && g.landed && g.reactT > REACT_DUR) {
                g.kick++;
                int left = KICKS_TOTAL - g.kick;
                bool decided = (g.scored > g.target) || (g.scored + left <= g.target) || (g.kick >= KICKS_TOTAL);
                if (decided) {
                    g.roundWon = (g.scored > g.target);
                    if (!g.roundWon)             g.gameLost = true;
                    else if (g.round >= ROUNDS-1) g.gameWon = true;
                    g.phase = PHASE_RESULT;
                } else StartKick();
            }
        }
    } break;

    case PHASE_RESULT:
        StepNet(sdt); KeeperReact(sdt); SpinBall(sdt);
        if (IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (g.gameWon || g.gameLost) ResetMatch();
            else { g.round++; SetupRound(g.round); }     // round cleared -> next round
        }
        break;
    }

    DriveCamera(dt);

    // ---- draw ----
    int W = GetScreenWidth(), H = GetScreenHeight();
    BeginDrawing();
    ClearBackground((Color){ 14, 16, 26, 255 });
    DrawRectangleGradientV(0, 0, W, H, (Color){ 26, 30, 56, 255 }, (Color){ 58, 48, 70, 255 });   // dusk sky
    BeginMode3D(g.cam);
        DrawStadium();
        DrawBanners();
        DrawPlane((Vector3){0, 0, 9}, (Vector2){44, 34}, (Color){ 34, 78, 46, 255 });
        DrawPitch();
        DrawCube((Vector3){-GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){ GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){0, GOAL_H, GOAL_Z}, GOAL_HALF_W*2, POST_R*2, POST_R*2, RAYWHITE);
        DrawNet();
        DrawKeeper();
        gBallModel.transform = QuaternionToMatrix(gBallRot);
        DrawModel(gBallModel, g.ball.pos, 1.0f, WHITE);
        if (g.phase == PHASE_AIM || g.phase == PHASE_CHARGE) {
            Strike s = { .power = 13.0f + g.power01*16.0f, .yaw = g.yaw, .pitch = g.pitch, .curve = g.curve*3.0f };
            Ball tb = BallLaunch((Vector3){0, BALL_R, 0}, s);
            Vector3 prev = tb.pos;
            for (int i = 0; i < 60 && tb.pos.z < GOAL_Z + 1; i++) { BallStep(&tb, 0.03f); DrawLine3D(prev, tb.pos, (Color){255,90,90,150}); prev = tb.pos; }
        }
    EndMode3D();

    DrawBoardOverlay();

    // ---- minimal HUD — the match now lives on the jumbotron ----
    int f = (int)Clamp(H*0.030f, 13.0f, 24.0f);
    int mg = (int)(H*0.035f);
    if (g.phase == PHASE_CHARGE) {
        int bw = (int)(W*0.42f), bh = (int)(f*1.1f), bx = W/2 - bw/2, by = H - mg - bh;
        DrawRectangle(bx, by, bw, bh, (Color){0,0,0,120});
        DrawRectangle(bx, by, (int)(bw*g.power01), bh, (Color){90,200,120,255});
    } else {
        const char *hint = (g.phase == PHASE_RESULT) ? "Tap to play again"
                         : (g.phase == PHASE_AIM)    ? "Hold & aim, release to shoot  ·  drag to curl" : "";
        if (hint[0]) {
            int fs = f; while (fs > 10 && MeasureText(hint, fs) > W - 2*mg) fs--;
            DrawText(hint, W/2 - MeasureText(hint, fs)/2, H - mg - fs, fs, (Color){210,216,224,220});
        }
    }

    // ---- bullet-time replay: letterbox + badge ----
    if (g.cine) {
        int bh = (int)(H * 0.09f * Clamp(fminf(g.cineT, g.cineDur - g.cineT) / 0.25f, 0.0f, 1.0f));
        DrawRectangle(0, 0, W, bh, (Color){0,0,0,235});
        DrawRectangle(0, H - bh, W, bh, (Color){0,0,0,235});
        if (bh > 5) {
            int fs = (int)Clamp(H*0.030f, 12.0f, 26.0f);
            bool blink = fmodf(g.cineT, 0.7f) < 0.45f;
            DrawText("REPLAY", (int)(W*0.045f) + fs + 6, bh/2 - fs/2, fs, (Color){232,236,242,235});
            if (blink) DrawCircle((int)(W*0.045f) + fs/2, bh/2, fs*0.32f, (Color){235,80,80,255});
        }
    }
    EndDrawing();
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(900, 600, "Arcade - World Cup Penalty");
    SetRandomSeed((unsigned int)time(NULL));   // fresh matchups every session

    LoadArt();                        // bake ball skin + keeper faces (needs the GL context)

    g.cam = (Camera3D){0};
    g.cam.position   = CAM_POS;
    g.cam.target     = CAM_TGT;
    g.cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    g.cam.fovy       = 55.0f;
    g.cam.projection = CAMERA_PERSPECTIVE;

    ResetMatch();

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) UpdateDrawFrame();
#endif
    CloseWindow();
    return 0;
}
