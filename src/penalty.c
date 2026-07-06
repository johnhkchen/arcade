// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core.
//
// Physics, not heuristics: the ball is a sphere that collides with a *diving*
// keeper (reaction delay + finite dive speed + limited reach) and with the
// posts/bar; the kick trades power against accuracy and curls with real
// sidespin. The keeper's DECISION stays dumb (a guess with error) — only its
// MOTION is physical, so a well-placed shot beats it by geometry, not luck.
//
// Flow per kick: AIM (arrows aim, A/D set curl) -> hold SPACE to charge power,
// release to strike -> ball flies & collides -> GOAL / SAVE / POST / MISS.
// Five kicks, running tally.
#include "raylib.h"
#include "raymath.h"
#include "ballstrike.h"
#include <math.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

#define GOAL_Z         11.0f   /* ~12 yds — regulation penalty distance */
#define GOAL_HALF_W     3.66f   // half of a 7.32m goal
#define GOAL_H          2.44f
#define BALL_R          0.11f
#define POST_R          0.06f
#define KICKS_TOTAL     5

// keeper
#define KPR_REACH       1.05f   // body+arm reach radius
#define KPR_DIVE_SPEED  8.5f     // m/s, finite — this is what makes corners score
#define KPR_ACCEL      55.0f
#define KPR_HOME_Y      1.05f

typedef enum { PHASE_AIM, PHASE_CHARGE, PHASE_FLY, PHASE_RESULT } Phase;
typedef enum { RES_NONE, RES_GOAL, RES_SAVE, RES_POST, RES_MISS } Result;

typedef struct {
    Phase   phase;
    float   yaw, pitch, curve;     // aim + curl input
    float   power01, chargeDir;    // charge meter
    Ball    ball;
    // keeper (physical dive)
    Vector3 kprPos, kprVel;
    float   kprReact;              // reaction countdown before it can move
    float   kprDiveX, kprDiveH;    // chosen dive target (a guess, with error)
    // scoring
    int     kick, scored;
    Result  result;
    float   resultTimer;
    Camera3D cam;
} Game;

static Game g;

static float Frand(void) { return GetRandomValue(0, 1000) / 1000.0f; }   // 0..1
static float Frand2(void) { return (Frand() - 0.5f) * 2.0f; }             // -1..1

static void ResetKeeper(void)
{
    g.kprPos = (Vector3){ 0.0f, KPR_HOME_Y, GOAL_Z - 0.5f };
    g.kprVel = (Vector3){0};
    g.kprReact = 0.0f;
    g.kprDiveX = 0.0f;
    g.kprDiveH = KPR_HOME_Y;
}

static void StartKick(void)
{
    g.phase = PHASE_AIM;
    g.yaw = 0.0f; g.pitch = 0.16f; g.curve = 0.0f;
    g.power01 = 0.0f; g.chargeDir = 1.0f;
    g.ball = (Ball){0};
    g.ball.pos = (Vector3){0.0f, BALL_R, 0.0f};
    ResetKeeper();
    g.result = RES_NONE;
    g.resultTimer = 0.0f;
}

static void ResetMatch(void) { g.kick = 0; g.scored = 0; StartKick(); }

static Strike AimStrike(void)  // the strike the current aim/power would produce
{
    // Power trades against accuracy: a harder shot scatters more.
    float scatter = g.power01 * 0.05f;
    return (Strike){
        .power = 13.0f + g.power01 * 16.0f,
        .yaw   = g.yaw   + Frand2() * scatter,
        .pitch = g.pitch + Frand2() * scatter * 0.6f,
        .curve = g.curve * 3.0f,
    };
}

// Roughly where will the ball cross the goal plane? (keeper's imperfect read)
static void PredictCrossing(float *px, float *py)
{
    Ball p = g.ball;
    for (int i = 0; i < 500 && p.pos.z < GOAL_Z; i++) BallStep(&p, 0.01f);
    *px = p.pos.x; *py = p.pos.y;
}

static void CommitKeeperDive(void)
{
    float px, py; PredictCrossing(&px, &py);
    // dumb read: partial confidence + error, sometimes wrong-footed entirely
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
    g.kprVel.y -= 3.5f * dt;                                    // the dive arcs down
    g.kprPos = Vector3Add(g.kprPos, Vector3Scale(g.kprVel, dt));
    if (g.kprPos.y < 0.45f) { g.kprPos.y = 0.45f; g.kprVel.y = 0.0f; }
}

// advance ball with collisions; sets g.result when the shot resolves
static void StepBall(float dt)
{
    const int N = 8;
    for (int i = 0; i < N && g.result == RES_NONE; i++) {
        BallStep(&g.ball, dt / N);
        Vector3 p = g.ball.pos;

        // keeper parry (only near the line)
        if (p.z > GOAL_Z - 2.2f && Vector3Distance(p, g.kprPos) < BALL_R + KPR_REACH) {
            Vector3 n = Vector3Normalize(Vector3Subtract(p, g.kprPos));
            g.ball.vel = Vector3Scale(Vector3Reflect(g.ball.vel, n), 0.45f);
            g.result = RES_SAVE;
            break;
        }
        // posts & crossbar
        if (p.z >= GOAL_Z - 0.12f && p.z <= GOAL_Z + 0.2f) {
            bool hitPost = (fabsf(fabsf(p.x) - GOAL_HALF_W) < BALL_R + POST_R) && p.y > 0 && p.y < GOAL_H + 0.1f;
            bool hitBar  = (fabsf(p.y - GOAL_H) < BALL_R + POST_R) && fabsf(p.x) < GOAL_HALF_W;
            if (hitPost || hitBar) { g.result = RES_POST; break; }
        }
        // goal line
        if (p.z >= GOAL_Z) {
            bool in = fabsf(p.x) < GOAL_HALF_W && p.y > 0 && p.y < GOAL_H;
            g.result = in ? RES_GOAL : RES_MISS;
            break;
        }
        // turf: bounce when descending fast, otherwise settle and ROLL toward goal
        if (g.ball.pos.y <= BALL_R) {
            g.ball.pos.y = BALL_R;
            if (g.ball.vel.y < -1.6f) {                  // a real bounce
                g.ball.vel.y *= -0.45f;
                g.ball.vel.x *= 0.9f; g.ball.vel.z *= 0.9f;
            } else {                                      // rolling on the grass
                g.ball.vel.y = 0.0f;
                float roll = 1.0f - 0.35f * (dt / N);     // light rolling friction
                g.ball.vel.x *= roll; g.ball.vel.z *= roll;
            }
        }
    }
    // a MISS only once it has actually stopped on the grass short of goal
    if (g.result == RES_NONE && g.ball.pos.z < GOAL_Z - 0.3f &&
        g.ball.pos.y <= BALL_R + 0.05f && Vector3Length(g.ball.vel) < 0.5f)
        g.result = RES_MISS;
}

static void ResolveResult(void)
{
    if (g.result == RES_GOAL) g.scored++;
    g.ball.live = false;
    g.phase = PHASE_RESULT;
    g.resultTimer = 0.0f;
}

static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;   // clamp big hitches (tab refocus)

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
        StepBall(dt);
        if (g.result != RES_NONE) ResolveResult();
    } break;

    case PHASE_RESULT: {
        g.resultTimer += dt;
        if (g.resultTimer > 1.7f) {
            g.kick++;
            if (g.kick >= KICKS_TOTAL) { if (IsKeyPressed(KEY_ENTER)) ResetMatch(); }
            else StartKick();
        }
    } break;
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
        for (int i = -3; i <= 3; i++)
            DrawLine3D((Vector3){i*GOAL_HALF_W/3, 0, GOAL_Z}, (Vector3){i*GOAL_HALF_W/3, GOAL_H, GOAL_Z}, (Color){200,200,200,80});
        // keeper: body + reach hint
        DrawCube(g.kprPos, 0.55f, 1.05f, 0.4f, (Color){ 240, 190, 40, 255 });
        DrawSphereWires(g.kprPos, KPR_REACH, 6, 6, (Color){ 240, 190, 40, 45 });
        // ball
        DrawSphere(g.ball.pos, BALL_R * 3.0f, RAYWHITE);
        // predicted path while aiming
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
    if (g.phase == PHASE_RESULT) {
        const char *msg = g.result==RES_GOAL ? "GOAL!" : g.result==RES_SAVE ? "SAVED!" :
                          g.result==RES_POST ? "OFF THE WOODWORK!" : "MISS!";
        Color c = g.result==RES_GOAL ? (Color){90,220,120,255} : (Color){235,90,90,255};
        int fs = 56; int w = MeasureText(msg, fs);
        DrawText(msg, GetScreenWidth()/2 - w/2, 92, fs, c);
        if (g.kick >= KICKS_TOTAL-1 && g.resultTimer > 1.7f)
            DrawText("ENTER to play again", GetScreenWidth()/2 - 110, 168, 20, RAYWHITE);
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
