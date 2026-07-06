// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core.
//
// Physics, not heuristics:
//  - the keeper is a diving body with two FLOATING GLOVES + a torso, each a real
//    sphere collider; gloves reach toward the ball (capped by arm length) and
//    punch it away with the dive's momentum — so a shot placed beyond their reach
//    scores by geometry, and a parry actually deflects.
//  - the ball keeps living after it's judged: it flies into a NET volume, the net
//    drags it down and it nestles — no instant pause. Same for a ricocheting save.
//
// Per kick: AIM (arrows aim, A/D curl) -> hold SPACE to charge power, release to
// strike -> ball flies, collides, and settles -> GOAL/SAVE/WOODWORK/MISS. 5 kicks.
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

#define KPR_DIVE_SPEED  8.5f
#define KPR_ACCEL      55.0f
#define KPR_HOME_Y      1.05f
#define KPR_BODY        0.42f   // torso collider radius
#define GLOVE_R         0.17f
#define ARM             0.80f   // glove reach from shoulder
#define SHOULDER        0.32f
#define SHOULDER_Y      0.25f
#define SETTLE_TIME     1.5f

typedef enum { PHASE_AIM, PHASE_CHARGE, PHASE_FLY, PHASE_RESULT } Phase;
typedef enum { RES_NONE, RES_GOAL, RES_SAVE, RES_POST, RES_MISS } Result;

typedef struct {
    Phase   phase;
    float   yaw, pitch, curve;
    float   power01, chargeDir;
    Ball    ball;
    // keeper (diving body + floating gloves)
    Vector3 kprPos, kprVel;
    float   kprReact, kprDiveX, kprDiveH;
    // scoring / flow
    int     kick, scored;
    Result  result;
    bool    resolved;          // judged, but ball still settling
    float   resultTimer;
    Camera3D cam;
} Game;

static Game g;

static float Frand(void)  { return GetRandomValue(0, 1000) / 1000.0f; }
static float Frand2(void) { return (Frand() - 0.5f) * 2.0f; }

// A glove: reaches from a shoulder toward the ball, capped at arm's length.
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
}

static void StartKick(void)
{
    g.phase = PHASE_AIM;
    g.yaw = 0.0f; g.pitch = 0.16f; g.curve = 0.0f;
    g.power01 = 0.0f; g.chargeDir = 1.0f;
    g.ball = (Ball){0};
    g.ball.pos = (Vector3){0.0f, BALL_R, 0.0f};
    ResetKeeper();
    g.result = RES_NONE; g.resolved = false; g.resultTimer = 0.0f;
}

static void ResetMatch(void) { g.kick = 0; g.scored = 0; StartKick(); }

static Strike AimStrike(void)
{
    float scatter = g.power01 * 0.05f;   // harder shots scatter more
    return (Strike){
        .power = 13.0f + g.power01 * 16.0f,
        .yaw   = g.yaw   + Frand2() * scatter,
        .pitch = g.pitch + Frand2() * scatter * 0.6f,
        .curve = g.curve * 3.0f,
    };
}

static void PredictCrossing(float *px, float *py)
{
    Ball p = g.ball;
    for (int i = 0; i < 500 && p.pos.z < GOAL_Z; i++) BallStep(&p, 0.01f);
    *px = p.pos.x; *py = p.pos.y;
}

static void CommitKeeperDive(void)
{
    float px, py; PredictCrossing(&px, &py);
    float err = Frand2() * 1.9f;
    if (Frand() < 0.18f) err += (px > 0 ? -1.0f : 1.0f) * 2.5f;   // occasional wrong guess
    g.kprDiveX = Clamp(px * 0.8f + err, -GOAL_HALF_W - 0.6f, GOAL_HALF_W + 0.6f);
    g.kprDiveH = Clamp(py * 0.85f + 0.35f, 0.5f, GOAL_H);
    g.kprReact = 0.16f + Frand() * 0.12f;
    g.kprVel = (Vector3){0};
}

static void StepKeeper(float dt)
{
    if (g.kprReact > 0.0f) { g.kprReact -= dt; return; }
    Vector3 target = (Vector3){ g.kprDiveX, g.kprDiveH, GOAL_Z - 0.45f };
    Vector3 to = Vector3Subtract(target, g.kprPos);
    if (Vector3Length(to) > 0.02f)
        g.kprVel = Vector3Add(g.kprVel, Vector3Scale(Vector3Normalize(to), KPR_ACCEL * dt));
    float sp = Vector3Length(g.kprVel);
    if (sp > KPR_DIVE_SPEED) g.kprVel = Vector3Scale(g.kprVel, KPR_DIVE_SPEED / sp);
    g.kprVel.y -= 3.5f * dt;
    g.kprPos = Vector3Add(g.kprPos, Vector3Scale(g.kprVel, dt));
    if (g.kprPos.y < 0.45f) { g.kprPos.y = 0.45f; g.kprVel.y = 0.0f; }
}

// gloves + torso; deflects the ball (punch along the dive) and returns true on contact
static bool KeeperDeflect(void)
{
    if (g.ball.pos.z < GOAL_Z - 1.6f) return false;   // only near the line
    Vector3 col[3] = { g.kprPos, GlovePos(-1), GlovePos(+1) };
    float    rad[3] = { KPR_BODY, GLOVE_R, GLOVE_R };
    for (int i = 0; i < 3; i++) {
        Vector3 d = Vector3Subtract(g.ball.pos, col[i]);
        float len = Vector3Length(d);
        if (len < BALL_R + rad[i]) {
            Vector3 n = (len > 0.001f) ? Vector3Scale(d, 1.0f/len) : (Vector3){0, 0, -1};
            float punch = fmaxf(0.0f, Vector3DotProduct(g.kprVel, n));
            g.ball.vel = Vector3Add(Vector3Scale(Vector3Reflect(g.ball.vel, n), 0.5f),
                                    Vector3Scale(n, punch * 0.9f + 2.5f));
            return true;
        }
    }
    return false;
}

static void Resolve(Result r) { g.result = r; g.resolved = true; g.resultTimer = 0.0f; }

// pre-judgment flight: collisions decide the outcome
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
            if (in) { g.scored++; Resolve(RES_GOAL); } else Resolve(RES_MISS);
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

// post-judgment settle: ball keeps living — net catches it / a save ricochets
static void StepBallSettle(float dt)
{
    const int N = 6;
    for (int i = 0; i < N; i++) {
        BallStep(&g.ball, dt / N);
        if (g.ball.pos.z > GOAL_Z) {                              // inside the net volume
            g.ball.vel = Vector3Scale(g.ball.vel, 1.0f - 3.5f*(dt/N));
            if (g.ball.pos.z > GOAL_Z + NET_DEPTH) { g.ball.pos.z = GOAL_Z + NET_DEPTH; g.ball.vel.z *= -0.2f; }
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

static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;

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
        if (IsKeyPressed(KEY_ENTER)) ResetMatch();
        break;
    }

    // ---- draw ----
    BeginDrawing();
    ClearBackground((Color){ 22, 26, 34, 255 });

    BeginMode3D(g.cam);
        DrawPlane((Vector3){0, 0, 9}, (Vector2){40, 30}, (Color){ 34, 78, 46, 255 });
        DrawCircle3D((Vector3){0, 0.01f, 0}, 0.22f, (Vector3){1,0,0}, 90.0f, RAYWHITE);

        // goal frame
        DrawCube((Vector3){-GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){ GOAL_HALF_W, GOAL_H/2, GOAL_Z}, POST_R*2, GOAL_H, POST_R*2, RAYWHITE);
        DrawCube((Vector3){0, GOAL_H, GOAL_Z}, GOAL_HALF_W*2, POST_R*2, POST_R*2, RAYWHITE);

        // net: mouth grid, back wall, depth edges
        {
            Color nc = (Color){ 210, 215, 220, 55 };
            float bz = GOAL_Z + NET_DEPTH;
            for (int i = -3; i <= 3; i++) {
                float x = i*GOAL_HALF_W/3;
                DrawLine3D((Vector3){x,0,GOAL_Z}, (Vector3){x,GOAL_H,GOAL_Z}, nc);
                DrawLine3D((Vector3){x,0,bz},      (Vector3){x,GOAL_H,bz},     nc);
            }
            for (int j = 0; j <= 3; j++) {
                float y = j*GOAL_H/3;
                DrawLine3D((Vector3){-GOAL_HALF_W,y,bz}, (Vector3){GOAL_HALF_W,y,bz}, nc);
            }
            DrawLine3D((Vector3){-GOAL_HALF_W,GOAL_H,GOAL_Z}, (Vector3){-GOAL_HALF_W,GOAL_H,bz}, nc);
            DrawLine3D((Vector3){ GOAL_HALF_W,GOAL_H,GOAL_Z}, (Vector3){ GOAL_HALF_W,GOAL_H,bz}, nc);
        }

        // keeper: torso + two floating gloves
        DrawCube(g.kprPos, 0.5f, KPR_BODY*2.2f, 0.35f, (Color){ 240, 190, 40, 255 });
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
