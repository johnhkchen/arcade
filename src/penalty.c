// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core.
//
//  - keeper: two floating GLOVE colliders + torso on a body that DIVES ONCE,
//    ballistically — it launches with a capped impulse and cannot recover, so a
//    shot placed past its committed reach scores. Gloves punch with dive momentum.
//  - net: a real Verlet mass-spring cloth pinned to the frame; the ball pushes it,
//    it bulges and catches, then springs back. Ball keeps living after judgment.
//  - a light crowd in the stand jumps to celebrate a goal.
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

#define KPR_DIVE_SPEED  8.5f    // capped horizontal dive speed — corners beat it
#define KPR_HOME_Y      1.05f
#define KPR_BODY        0.42f
#define GLOVE_R         0.17f
#define ARM             0.80f
#define SHOULDER        0.32f
#define SHOULDER_Y      0.25f
#define SETTLE_TIME     1.6f

#define NET_NX         17
#define NET_NY         11

typedef enum { PHASE_AIM, PHASE_CHARGE, PHASE_FLY, PHASE_RESULT } Phase;
typedef enum { RES_NONE, RES_GOAL, RES_SAVE, RES_POST, RES_MISS } Result;

typedef struct { Vector3 p, prev; bool pin; } NetNode;

typedef struct {
    Phase   phase;
    float   yaw, pitch, curve;
    float   power01, chargeDir;
    Ball    ball;
    // keeper
    Vector3 kprPos, kprVel;
    float   kprReact, kprDiveX, kprDiveH;
    bool    kprLaunched;
    // flow
    int     kick, scored;
    Result  result;
    bool    resolved;
    float   resultTimer, celebrate;
    Camera3D cam;
} Game;

static Game    g;
static NetNode gNet[NET_NY][NET_NX];
static float   gNetDx, gNetDy;

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
            n->p.y -= 9.5f * dt2;                              // a little more drape
            n->prev = cur;
        }
    // ball pushes the net backward (bulge)
    bool inGoal = g.ball.pos.z > GOAL_Z - 0.3f && fabsf(g.ball.pos.x) < GOAL_HALF_W + 0.3f && g.ball.pos.y < GOAL_H + 0.3f;
    if (inGoal)
        for (int r = 0; r < NET_NY; r++)
            for (int c = 0; c < NET_NX; c++) {
                NetNode *n = &gNet[r][c];
                if (n->pin) continue;
                float dx = n->p.x - g.ball.pos.x, dy = n->p.y - g.ball.pos.y;
                if (dx*dx + dy*dy < 0.55f && g.ball.pos.z + BALL_R > n->p.z)
                    n->p.z += (g.ball.pos.z + BALL_R*1.4f - n->p.z) * 0.6f;   // soft give
            }
    // relax springs
    for (int it = 0; it < 2; it++) {                          // fewer iters = slacker
        for (int r = 0; r < NET_NY; r++)
            for (int c = 0; c < NET_NX; c++) {
                if (c+1 < NET_NX) Relax(&gNet[r][c], &gNet[r][c+1], gNetDx * 1.06f);
                if (r+1 < NET_NY) Relax(&gNet[r][c], &gNet[r+1][c], gNetDy * 1.06f);
            }
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
    g.kprReact = 0.0f; g.kprDiveX = 0.0f; g.kprDiveH = KPR_HOME_Y;
    g.kprLaunched = false;
}

// one-time ballistic dive launch; capped so far/high corners can't be reached
static void LaunchDive(void)
{
    Vector3 to = (Vector3){ g.kprDiveX - g.kprPos.x, 0.0f, (GOAL_Z - 0.45f) - g.kprPos.z };
    float d = Vector3Length(to);
    Vector3 dir = (d > 1e-4f) ? Vector3Scale(to, 1.0f/d) : (Vector3){0,0,0};
    float hspeed = fminf(d / 0.32f, KPR_DIVE_SPEED);
    g.kprVel = Vector3Scale(dir, hspeed);
    g.kprVel.y = Clamp((g.kprDiveH - g.kprPos.y) / 0.32f + 2.2f, -1.0f, 6.5f);
    g.kprLaunched = true;
}

static void StepKeeper(float dt)
{
    if (!g.kprLaunched) {
        if (g.kprReact > 0.0f) { g.kprReact -= dt; if (g.kprReact <= 0.0f) LaunchDive(); }
        return;
    }
    // committed: gravity only, no steering, no recovery
    g.kprVel.y -= 11.0f * dt;
    g.kprPos = Vector3Add(g.kprPos, Vector3Scale(g.kprVel, dt));
    if (g.kprPos.y < 0.42f) { g.kprPos.y = 0.42f; g.kprVel.y = 0.0f; g.kprVel.x *= 0.9f; g.kprVel.z *= 0.9f; }
}

static bool KeeperDeflect(void)
{
    if (g.ball.pos.z < GOAL_Z - 1.6f) return false;
    Vector3 col[3] = { g.kprPos, GlovePos(-1), GlovePos(+1) };
    float    rad[3] = { KPR_BODY, GLOVE_R, GLOVE_R };
    for (int i = 0; i < 3; i++) {
        Vector3 d = Vector3Subtract(g.ball.pos, col[i]);
        float len = Vector3Length(d);
        if (len < BALL_R + rad[i]) {
            Vector3 n = (len > 1e-3f) ? Vector3Scale(d, 1.0f/len) : (Vector3){0, 0, -1};
            float punch = fmaxf(0.0f, Vector3DotProduct(g.kprVel, n));
            g.ball.vel = Vector3Add(Vector3Scale(Vector3Reflect(g.ball.vel, n), 0.5f),
                                    Vector3Scale(n, punch * 0.9f + 2.5f));
            return true;
        }
    }
    return false;
}

// ---- flow ---------------------------------------------------------------
static void StartKick(void)
{
    g.phase = PHASE_AIM;
    g.yaw = 0.0f; g.pitch = 0.16f; g.curve = 0.0f;
    g.power01 = 0.0f; g.chargeDir = 1.0f;
    g.ball = (Ball){0};
    g.ball.pos = (Vector3){0.0f, BALL_R, 0.0f};
    ResetKeeper();
    InitNet();
    g.result = RES_NONE; g.resolved = false; g.resultTimer = 0.0f;
}

static void ResetMatch(void) { g.kick = 0; g.scored = 0; g.celebrate = 0.0f; StartKick(); }

static Strike AimStrike(void)
{
    float scatter = g.power01 * 0.05f;
    return (Strike){
        .power = 13.0f + g.power01 * 16.0f,
        .yaw   = g.yaw   + Frand2() * scatter,
        .pitch = g.pitch + Frand2() * scatter * 0.6f,
        .curve = g.curve * 3.0f,
    };
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
    g.kprReact = 0.16f + Frand() * 0.12f;
    g.kprLaunched = false;
    g.kprVel = (Vector3){0};
}

static void Resolve(Result r)
{
    g.result = r; g.resolved = true; g.resultTimer = 0.0f;
    if (r == RES_GOAL) { g.scored++; g.celebrate = 1.7f; }
}

static void StepBallLive(float dt)
{
    const int N = 8;
    for (int i = 0; i < N; i++) {
        BallStep(&g.ball, dt / N);
        Vector3 p = g.ball.pos;
        if (KeeperDeflect()) { Resolve(RES_SAVE); return; }
        if (p.z >= GOAL_Z - 0.12f && p.z <= GOAL_Z + 0.2f) {
            bool hitPost = (fabsf(fabsf(p.x) - GOAL_HALF_W) < BALL_R + POST_R) && p.y > 0 && p.y < GOAL_H + 0.1f;
            bool hitBar  = (fabsf(p.y - GOAL_H) < BALL_R + POST_R) && fabsf(p.x) < GOAL_HALF_W;
            if (hitPost || hitBar) {
                if (hitPost) g.ball.vel.x *= -0.5f;
                if (hitBar)  g.ball.vel.y *= -0.5f;
                Resolve(RES_POST); return;
            }
        }
        if (p.z >= GOAL_Z) {
            bool in = fabsf(p.x) < GOAL_HALF_W && p.y > 0 && p.y < GOAL_H;
            Resolve(in ? RES_GOAL : RES_MISS);
            return;
        }
        if (g.ball.pos.y <= BALL_R) {
            g.ball.pos.y = BALL_R;
            if (g.ball.vel.y < -1.6f) { g.ball.vel.y *= -0.45f; g.ball.vel.x *= 0.9f; g.ball.vel.z *= 0.9f; }
            else { g.ball.vel.y = 0.0f; float roll = 1.0f - 0.35f*(dt/N); g.ball.vel.x *= roll; g.ball.vel.z *= roll; }
        }
    }
    if (g.ball.pos.z < GOAL_Z - 0.3f && g.ball.pos.y <= BALL_R + 0.05f && Vector3Length(g.ball.vel) < 0.5f)
        Resolve(RES_MISS);
}

static void StepBallSettle(float dt)
{
    const int N = 6;
    float backZ = GOAL_Z + NET_DEPTH;
    for (int i = 0; i < N; i++) {
        BallStep(&g.ball, dt / N);
        if (g.ball.pos.z > GOAL_Z) {
            g.ball.vel = Vector3Scale(g.ball.vel, 1.0f - 4.5f*(dt/N));   // net absorbs
            if (g.ball.pos.z > backZ + 0.35f) { g.ball.pos.z = backZ + 0.35f; if (g.ball.vel.z > 0) g.ball.vel.z *= -0.12f; }
            if (g.ball.pos.x >  GOAL_HALF_W - BALL_R) { g.ball.pos.x =  GOAL_HALF_W - BALL_R; g.ball.vel.x *= -0.2f; }
            if (g.ball.pos.x < -GOAL_HALF_W + BALL_R) { g.ball.pos.x = -GOAL_HALF_W + BALL_R; g.ball.vel.x *= -0.2f; }
            if (g.ball.pos.y >  GOAL_H - BALL_R)      { g.ball.pos.y =  GOAL_H - BALL_R;      g.ball.vel.y *= -0.2f; }
        }
        if (g.ball.pos.y <= BALL_R) {
            g.ball.pos.y = BALL_R;
            if (g.ball.vel.y < -1.6f) { g.ball.vel.y *= -0.45f; g.ball.vel.x *= 0.9f; g.ball.vel.z *= 0.9f; }
            else { g.ball.vel.y = 0.0f; float roll = 1.0f - 0.9f*(dt/N); g.ball.vel.x *= roll; g.ball.vel.z *= roll; }
        }
    }
}

// ---- crowd --------------------------------------------------------------
static Color CrowdColor(int h)
{
    static const Color pal[5] = {
        {200,70,70,255}, {238,238,238,255}, {70,110,200,255}, {235,200,60,255}, {80,180,110,255}
    };
    return pal[h % 5];
}

static void DrawCrowd(void)
{
    float t = GetTime();
    bool cel = g.celebrate > 0.0f;
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 44; col++) {
            int h = (row*31 + col*17) % 100;
            float x = -13.0f + col * (26.0f / 43.0f);
            float z = GOAL_Z + 3.0f + row * 0.95f;
            float y = 0.7f + row * 0.75f;
            float jump = cel ? fabsf(sinf(t*12.0f + h)) * 0.35f : 0.0f;
            DrawCube((Vector3){ x, y + jump, z }, 0.28f, 0.42f, 0.28f, CrowdColor(h));
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
    PitchLineH(GOAL_Z, -PW, PW);            // goal line
    PitchLineH(-5.5f,  -PW, PW);            // penalty area front (16.5m)
    PitchLineV(-PW, -5.5f, GOAL_Z);
    PitchLineV( PW, -5.5f, GOAL_Z);
    PitchLineH(5.5f, -GW, GW);              // goal area / 6-yard box front (5.5m)
    PitchLineV(-GW, 5.5f, GOAL_Z);
    PitchLineV( GW, 5.5f, GOAL_Z);
    DrawCube((Vector3){0, 0.02f, 0}, 0.22f, 0.03f, 0.22f, (Color){235,238,240,220}); // penalty spot
}

// ---- main frame ---------------------------------------------------------
static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;
    if (g.celebrate > 0.0f) g.celebrate -= dt;

    switch (g.phase) {
    case PHASE_AIM: {
        float s = 0.9f * dt;
        if (IsKeyDown(KEY_LEFT))  g.yaw   -= s;
        if (IsKeyDown(KEY_RIGHT)) g.yaw   += s;
        if (IsKeyDown(KEY_UP))    g.pitch += s;
        if (IsKeyDown(KEY_DOWN))  g.pitch -= s;
        g.yaw = Clamp(g.yaw, -0.42f, 0.42f);
        g.pitch = Clamp(g.pitch, 0.02f, 0.55f);
        if (IsKeyDown(KEY_A)) g.curve = Clamp(g.curve - 1.4f*dt, -1.0f, 1.0f);
        if (IsKeyDown(KEY_D)) g.curve = Clamp(g.curve + 1.4f*dt, -1.0f, 1.0f);
        if (IsKeyPressed(KEY_SPACE)) { g.phase = PHASE_CHARGE; g.power01 = 0.0f; g.chargeDir = 1.0f; }
    } break;

    case PHASE_CHARGE: {
        g.power01 += g.chargeDir * 1.6f * dt;
        if (g.power01 >= 1.0f) { g.power01 = 1.0f; g.chargeDir = -1.0f; }
        if (g.power01 <= 0.0f) { g.power01 = 0.0f; g.chargeDir =  1.0f; }
        if (IsKeyReleased(KEY_SPACE) || IsKeyPressed(KEY_SPACE)) {
            g.ball = BallLaunch((Vector3){0, BALL_R, 0}, AimStrike());
            CommitKeeperDive();
            g.phase = PHASE_FLY;
        }
    } break;

    case PHASE_FLY: {
        StepKeeper(dt);
        StepNet(dt);
        if (!g.resolved) StepBallLive(dt);
        else             StepBallSettle(dt);
        if (g.resolved) {
            g.resultTimer += dt;
            if (g.resultTimer > SETTLE_TIME) {
                g.kick++;
                if (g.kick >= KICKS_TOTAL) g.phase = PHASE_RESULT;
                else StartKick();
            }
        }
    } break;

    case PHASE_RESULT:
        StepNet(dt);
        if (IsKeyPressed(KEY_ENTER)) ResetMatch();
        break;
    }

    // ---- draw ----
    BeginDrawing();
    ClearBackground((Color){ 20, 24, 32, 255 });

    BeginMode3D(g.cam);
        DrawCrowd();
        DrawPlane((Vector3){0, 0, 9}, (Vector2){44, 34}, (Color){ 34, 78, 46, 255 });
        DrawPitch();

        DrawCube((Vector3){-GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){ GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){0, GOAL_H, GOAL_Z}, GOAL_HALF_W*2, POST_R*2, POST_R*2, RAYWHITE);
        DrawNet();

        {   // keeper body: a capsule that lies along the dive — headfirst, not upright
            Vector3 axis = (Vector3){0, 1, 0};
            float sp = Vector3Length(g.kprVel);
            if (g.kprLaunched && sp > 1.0f) axis = Vector3Scale(g.kprVel, 1.0f/sp);
            Vector3 head = Vector3Add(g.kprPos, Vector3Scale(axis, 0.55f));
            Vector3 feet = Vector3Subtract(g.kprPos, Vector3Scale(axis, 0.55f));
            DrawCapsule(feet, head, 0.26f, 8, 8, (Color){ 240, 190, 40, 255 });
            DrawSphere(head, 0.17f, (Color){ 250, 225, 130, 255 });
        }
        DrawSphere(GlovePos(-1), GLOVE_R, (Color){ 40, 210, 235, 255 });
        DrawSphere(GlovePos(+1), GLOVE_R, (Color){ 40, 210, 235, 255 });
        DrawSphere(g.ball.pos, BALL_R * 3.0f, RAYWHITE);

        if (g.phase == PHASE_AIM || g.phase == PHASE_CHARGE) {
            Strike s = { .power = 13.0f + g.power01*16.0f, .yaw = g.yaw, .pitch = g.pitch, .curve = g.curve*3.0f };
            Ball t = BallLaunch((Vector3){0, BALL_R, 0}, s);
            Vector3 prev = t.pos;
            for (int i = 0; i < 60 && t.pos.z < GOAL_Z + 1; i++) {
                BallStep(&t, 0.03f);
                DrawLine3D(prev, t.pos, (Color){ 255, 90, 90, 150 });
                prev = t.pos;
            }
        }
    EndMode3D();

    // ---- HUD ----
    DrawText("WORLD CUP PENALTY", 20, 16, 28, RAYWHITE);
    DrawText(TextFormat("Kick %d / %d     Scored %d",
             (g.kick < KICKS_TOTAL ? g.kick+1 : KICKS_TOTAL), KICKS_TOTAL, g.scored),
             20, 50, 20, (Color){ 200, 210, 220, 255 });

    if (g.phase == PHASE_AIM)
        DrawText("Arrows: aim    A/D: curl    SPACE: charge", 20, GetScreenHeight()-34, 20, RAYWHITE);
    if (g.phase == PHASE_CHARGE) {
        DrawText("Release SPACE to strike   (more power = less accurate)", 20, GetScreenHeight()-34, 18, RAYWHITE);
        DrawRectangle(20, GetScreenHeight()-70, 300, 22, (Color){0,0,0,120});
        DrawRectangle(20, GetScreenHeight()-70, (int)(300*g.power01), 22, (Color){ 90, 200, 120, 255 });
    }
    if (g.phase == PHASE_FLY && g.resolved) {
        const char *msg = g.result==RES_GOAL ? "GOAL!" : g.result==RES_SAVE ? "SAVED!" :
                          g.result==RES_POST ? "OFF THE WOODWORK!" : "MISS!";
        Color c = g.result==RES_GOAL ? (Color){90,220,120,255} : (Color){235,90,90,255};
        int fs = 56; int w = MeasureText(msg, fs);
        DrawText(msg, GetScreenWidth()/2 - w/2, 92, fs, c);
    }
    if (g.phase == PHASE_RESULT) {
        const char *m = TextFormat("FULL TIME   Scored %d / %d", g.scored, KICKS_TOTAL);
        int w = MeasureText(m, 40);
        DrawText(m, GetScreenWidth()/2 - w/2, 96, 40, RAYWHITE);
        DrawText("ENTER to play again", GetScreenWidth()/2 - 110, 160, 20, (Color){200,210,220,255});
    }

    EndDrawing();
}

int main(void)
{
    InitWindow(900, 600, "Arcade - World Cup Penalty");

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

    CloseWindow();
    return 0;
}
