// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core.
#include "raylib.h"
#include "raymath.h"
#include "ballstrike.h"
#include <math.h>

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

#define NET_NX         17
#define NET_NY         11

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
    int     scrambleCount;
    float   scrambleCd;
    // keeper visual pose (sprung)
    Vector3 kprAxis, kprGL, kprGR, kprGLv, kprGRv, kprHeadOff, reactBase, reactAxis;
    int     reactAnim;
    bool    reactCelebrate, reactOnGround, landed, reactHeld;
    float   reactT;
    // flow
    int     kick, scored;
    Result  result, kickRes[KICKS_TOTAL];
    bool    resolved, caught, hitWood;
    float   resultTimer, celebrate, flight;
    // touch input
    bool    touchCharge;
    float   pressX;
    Camera3D cam;
} Game;

static Game    g;
static NetNode gNet[NET_NY][NET_NX];
static float   gNetDx, gNetDy;

typedef struct { Color a, b; } Team;   // primary, secondary kit colors
static const Team TEAMS[] = {
    {{247,203,44,255},{0,120,60,255}},   {{108,172,228,255},{245,245,245,255}},
    {{40,60,140,255},{205,30,45,255}},   {{224,60,50,255},{245,245,245,255}},
    {{255,120,20,255},{25,25,25,255}},   {{25,25,30,255},{230,230,230,255}},
    {{0,90,160,255},{240,220,40,255}},   {{150,20,50,255},{240,240,240,255}},
};
static Team gHome, gAway;
static RenderTexture2D gBoard;   // the jumbotron's live screen

static float Frand(void)  { return GetRandomValue(0, 1000) / 1000.0f; }
static float Frand2(void) { return (Frand() - 0.5f) * 2.0f; }

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
static Vector3 GlovePos(int side)
{
    Vector3 sh = Vector3Add(g.kprPos, (Vector3){ side * SHOULDER, SHOULDER_Y, 0.05f });
    Vector3 toBall = Vector3Subtract(g.ball.pos, sh);
    float d = Vector3Length(toBall);
    if (d > ARM) toBall = Vector3Scale(toBall, ARM / d);
    return Vector3Add(sh, toBall);
}
static void ResetKeeper(void)
{
    g.kprPos = (Vector3){ 0.0f, KPR_HOME_Y, GOAL_Z - 0.5f };
    g.kprVel = (Vector3){0};
    g.kprReact = 0.0f; g.kprDiveX = 0.0f; g.kprDiveH = KPR_HOME_Y; g.kprLaunched = false;
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
    g.kprVel = Vector3Scale(dir, fminf(d / 0.32f, KPR_DIVE_SPEED));
    g.kprVel.y = Clamp((g.kprDiveH - g.kprPos.y) / 0.32f + 2.2f, -1.0f, 6.5f);
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
static void UpdateGloves(float dt, Vector3 tAxis, Vector3 tGL, Vector3 tGR)
{
    g.kprAxis = Vector3Normalize(Vector3Lerp(g.kprAxis, tAxis, Clamp(13.0f*dt, 0.0f, 1.0f)));
    Spring(&g.kprGL, &g.kprGLv, tGL, dt, 130.0f, 15.0f);
    Spring(&g.kprGR, &g.kprGRv, tGR, dt, 130.0f, 15.0f);
}

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
        UpdateGloves(dt, tA, GlovePos(-1), GlovePos(+1));
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
}
static void ResetMatch(void)
{
    int n = (int)(sizeof(TEAMS)/sizeof(TEAMS[0]));
    int hi = GetRandomValue(0, n-1), ai = GetRandomValue(0, n-2);
    if (ai >= hi) ai++;                                  // distinct matchup
    gHome = TEAMS[hi]; gAway = TEAMS[ai];
    for (int i = 0; i < KICKS_TOTAL; i++) g.kickRes[i] = RES_NONE;
    g.kick = 0; g.scored = 0; g.celebrate = 0.0f; StartKick();
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
    float err = Frand2() * 1.9f;
    if (Frand() < 0.18f) err += (px > 0 ? -1.0f : 1.0f) * 2.5f;
    g.kprDiveX = Clamp(px * 0.8f + err, -GOAL_HALF_W - 0.6f, GOAL_HALF_W + 0.6f);
    g.kprDiveH = Clamp(py * 0.85f + 0.35f, 0.5f, GOAL_H);
    g.kprReact = 0.16f + Frand() * 0.12f; g.kprLaunched = false; g.kprVel = (Vector3){0};
}
static void Resolve(Result r)
{
    g.result = r; g.resolved = true; g.resultTimer = 0.0f;
    if (g.kick < KICKS_TOTAL) g.kickRes[g.kick] = r;
    if (r == RES_GOAL) { g.scored++; g.celebrate = 1.7f; }
    g.reactAnim = GetRandomValue(0, 4);
    g.reactCelebrate = (r != RES_GOAL);
    g.reactHeld = (r == RES_SAVE && g.caught);
    g.reactT = 0.0f; g.kprHeadOff = (Vector3){0};
    if (!g.kprLaunched || (g.kprPos.y > 0.85f && fabsf(g.kprVel.y) < 2.0f)) {
        g.landed = true;  g.reactOnGround = false; g.reactBase = (Vector3){ g.kprPos.x, 1.05f, g.kprPos.z }; g.reactAxis = (Vector3){0,1,0};
    } else { g.landed = false; g.reactOnGround = true; }
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
// render the live match onto the jumbotron texture (call before BeginDrawing)
static void RenderBoard(void)
{
    const int BW = 512, BH = 160;
    BeginTextureMode(gBoard);
    ClearBackground((Color){ 14, 16, 24, 255 });
    DrawRectangle(0, 0, 20, BH, gHome.a);      DrawRectangle(20, 0, 10, BH, gHome.b);
    DrawRectangle(BW-20, 0, 20, BH, gAway.a);  DrawRectangle(BW-30, 0, 10, BH, gAway.b);
    const char *hd = "PENALTY SHOOTOUT";
    DrawText(hd, BW/2 - MeasureText(hd, 24)/2, 12, 24, (Color){120,140,175,255});
    if (g.phase == PHASE_FLY && g.resolved) {
        const char *rw = g.result==RES_GOAL?"GOAL!":g.result==RES_SAVE?"SAVED!":g.result==RES_POST?"WOODWORK!":"MISS!";
        Color rc = g.result==RES_GOAL?(Color){90,220,120,255}:(Color){235,90,90,255};
        DrawText(rw, BW/2 - MeasureText(rw, 46)/2, 50, 46, rc);
    } else if (g.phase == PHASE_RESULT) {
        const char *ft = TextFormat("FULL TIME    %d / %d", g.scored, KICKS_TOTAL);
        DrawText(ft, BW/2 - MeasureText(ft, 38)/2, 54, 38, RAYWHITE);
    } else {
        const char *sc = TextFormat("SCORED  %d", g.scored);
        DrawText(sc, BW/2 - MeasureText(sc, 40)/2, 52, 40, RAYWHITE);
    }
    int gap = 40, tot = (KICKS_TOTAL-1)*gap, sx = BW/2 - tot/2, dy = 132;
    for (int i = 0; i < KICKS_TOTAL; i++) {
        int x = sx + i*gap;
        bool played = i < g.kick || (i == g.kick && g.resolved);
        Result rr = (i < g.kick) ? g.kickRes[i] : g.result;
        Color dc = played ? (rr==RES_GOAL ? (Color){90,220,120,255} : (Color){235,90,90,255}) : (Color){55,60,72,255};
        DrawCircle(x, dy, 10, dc);
        if (!played) DrawCircleLines(x, dy, 10, (Color){95,100,115,255});
    }
    EndTextureMode();
}
static void DrawScoreboard(void)
{
    Vector3 c = (Vector3){ 0.0f, 6.2f, GOAL_Z + 8.0f };
    DrawCube((Vector3){-6.1f, 3.1f, c.z + 0.12f}, 0.3f, 6.2f, 0.3f, (Color){40,42,50,255});
    DrawCube((Vector3){ 6.1f, 3.1f, c.z + 0.12f}, 0.3f, 6.2f, 0.3f, (Color){40,42,50,255});
    DrawCube((Vector3){0, c.y, c.z + 0.14f}, 12.6f, 4.1f, 0.25f, (Color){22,24,32,255});
    DrawBillboardRec(g.cam, gBoard.texture,
        (Rectangle){ 0, 0, (float)gBoard.texture.width, -(float)gBoard.texture.height },
        c, (Vector2){ 12.0f, 3.75f }, WHITE);
}
static void DrawStadium(void)
{
    float t = GetTime();
    bool cel = g.celebrate > 0.0f;
    const int ROWS = 7, COLS = 54;
    for (int r = 0; r < ROWS; r++) {
        float z = GOAL_Z + 2.7f + r*1.15f;
        float y = 0.2f + r*0.8f;
        DrawCube((Vector3){0, y - 0.5f, z}, 36.0f, 1.0f, 1.15f, (Color){ 58,60,70,255 });      // concrete tier
        DrawCube((Vector3){0, y - 0.02f, z - 0.5f}, 36.0f, 0.09f, 0.12f, (Color){ 40,42,50,255 }); // step nose
        for (int c = 0; c < COLS; c++) {
            int h = (r*37 + c*19) % 100;
            float x = -17.0f + c*(34.0f/(COLS-1));
            float jump = cel ? fabsf(sinf(t*12.0f + h))*0.32f : fabsf(sinf(t*1.1f + h))*0.03f;
            DrawCube((Vector3){x, y + 0.35f + jump, z}, 0.26f, 0.4f, 0.26f, FanColor(x, h));
        }
    }
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

// ---- main frame ---------------------------------------------------------
static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;
    if (g.celebrate > 0.0f) g.celebrate -= dt;
    if (g.scrambleCd > 0.0f) g.scrambleCd -= dt;
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
        StepNet(dt);
        if (!g.resolved) {
            StepKeeper(dt);
            TryScramble();                     // late lunge for a loose/slow ball
            { float sp = Vector3Length(g.kprVel);
              Vector3 tA = (g.kprLaunched && sp > 1.0f) ? Vector3Scale(g.kprVel, 1.0f/sp) : (Vector3){0,1,0};
              UpdateGloves(dt, tA, GlovePos(-1), GlovePos(+1)); }
            StepBallLive(dt);
            g.flight += dt;
            if (!g.resolved && g.flight > 5.0f) Resolve(g.hitWood ? RES_POST : RES_MISS);
        } else {
            KeeperReact(dt); StepBallSettle(dt); g.resultTimer += dt;
            if (g.landed && g.reactT > REACT_DUR) {
                g.kick++;
                if (g.kick >= KICKS_TOTAL) g.phase = PHASE_RESULT; else StartKick();
            }
        }
    } break;

    case PHASE_RESULT:
        StepNet(dt); KeeperReact(dt);
        if (IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ResetMatch();
        break;
    }

    // ---- draw ----
    RenderBoard();
    int W = GetScreenWidth(), H = GetScreenHeight();
    BeginDrawing();
    ClearBackground((Color){ 20, 24, 32, 255 });
    BeginMode3D(g.cam);
        DrawStadium();
        DrawScoreboard();
        DrawPlane((Vector3){0, 0, 9}, (Vector2){44, 34}, (Color){ 34, 78, 46, 255 });
        DrawPitch();
        DrawCube((Vector3){-GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){ GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){0, GOAL_H, GOAL_Z}, GOAL_HALF_W*2, POST_R*2, POST_R*2, RAYWHITE);
        DrawNet();
        {   // keeper — sprung pose (ragdoll feel)
            Vector3 axis = g.kprAxis;
            Vector3 head = Vector3Add(Vector3Add(g.kprPos, Vector3Scale(axis, 0.55f)), g.kprHeadOff);
            Vector3 feet = Vector3Subtract(g.kprPos, Vector3Scale(axis, 0.55f));
            Color body = gHome.a;                                   // home keeper kit
            if (g.resolved && !g.reactCelebrate)
                body = (Color){ (unsigned char)(body.r*0.68f), (unsigned char)(body.g*0.68f), (unsigned char)(body.b*0.68f), 255 };
            DrawCapsule(feet, head, 0.26f, 8, 8, body);
            DrawSphere(head, 0.17f, (Color){ 250, 225, 130, 255 });
            DrawSphere(g.kprGL, GLOVE_R, gHome.b);
            DrawSphere(g.kprGR, GLOVE_R, gHome.b);
        }
        DrawSphere(g.ball.pos, BALL_R * 3.0f, RAYWHITE);
        if (g.phase == PHASE_AIM || g.phase == PHASE_CHARGE) {
            Strike s = { .power = 13.0f + g.power01*16.0f, .yaw = g.yaw, .pitch = g.pitch, .curve = g.curve*3.0f };
            Ball tb = BallLaunch((Vector3){0, BALL_R, 0}, s);
            Vector3 prev = tb.pos;
            for (int i = 0; i < 60 && tb.pos.z < GOAL_Z + 1; i++) { BallStep(&tb, 0.03f); DrawLine3D(prev, tb.pos, (Color){255,90,90,150}); prev = tb.pos; }
        }
    EndMode3D();

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
    EndDrawing();
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(900, 600, "Arcade - World Cup Penalty");
    gBoard = LoadRenderTexture(512, 160);

    g.cam = (Camera3D){0};
    g.cam.position   = (Vector3){ 0.0f, 2.6f, -6.5f };
    g.cam.target     = (Vector3){ 0.0f, 1.4f, GOAL_Z };
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
    UnloadRenderTexture(gBoard);
    CloseWindow();
    return 0;
}
