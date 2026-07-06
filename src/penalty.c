// penalty.c — Arcade cartridge #1: World Cup Penalty Shootout.
// Skin #1 over the shared ballstrike core: aim + charge power + curl, beat the keeper.
//
// Flow per kick:  AIM (move reticle, A/D to set curl) -> hold SPACE to charge power,
// release to strike -> ball flies (ballstrike) -> GOAL / SAVE / MISS -> next kick.
// Five kicks, running tally. Keeper guesses a side at strike time (deliberately dumb —
// the fun is the skill shot, not a "realistic" AI).
#include "raylib.h"
#include "raymath.h"
#include "ballstrike.h"
#include <math.h>
#include <stdio.h>

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

// --- field constants (metres) ---
#define GOAL_Z        18.0f
#define GOAL_HALF_W    3.66f   // half of a 7.32m goal
#define GOAL_H         2.44f
#define BALL_R         0.11f
#define KICKS_TOTAL    5

typedef enum { PHASE_AIM, PHASE_CHARGE, PHASE_FLY, PHASE_RESULT } Phase;
typedef enum { RES_NONE, RES_GOAL, RES_SAVE, RES_MISS } Result;

typedef struct {
    Phase  phase;
    float  yaw;        // aim
    float  pitch;
    float  curve;      // -1..1
    float  power01;    // charge meter 0..1
    float  chargeDir;  // meter oscillation direction
    Ball   ball;
    // keeper
    float  keeperX;      // current x
    float  keeperTargetX;
    float  keeperY;      // dive height
    // scoring
    int    kick;         // 0-based
    int    scored;
    Result result;
    float  resultTimer;
    Camera3D cam;
} Game;

static Game g;

static void StartKick(void)
{
    g.phase = PHASE_AIM;
    g.yaw = 0.0f;
    g.pitch = 0.18f;
    g.curve = 0.0f;
    g.power01 = 0.0f;
    g.chargeDir = 1.0f;
    g.ball = (Ball){0};
    g.ball.pos = (Vector3){0.0f, BALL_R, 0.0f};
    g.keeperX = 0.0f;
    g.keeperTargetX = 0.0f;
    g.keeperY = 0.0f;
    g.result = RES_NONE;
    g.resultTimer = 0.0f;
}

static void ResetMatch(void)
{
    g.kick = 0;
    g.scored = 0;
    StartKick();
}

// deterministic-ish keeper guess without pulling in rand seeding fuss
static float KeeperGuess(void)
{
    // pseudo guess based on kick index + a time wobble; dumb on purpose
    float r = sinf((float)g.kick * 12.9898f + GetTime() * 0.37f) * 43758.5453f;
    r = r - floorf(r);            // 0..1
    return (r - 0.5f) * 2.0f * (GOAL_HALF_W * 0.9f);  // -x..x
}

static void Judge(void)
{
    // ball has crossed the goal plane; classify by where.
    float x = g.ball.pos.x;
    float y = g.ball.pos.y;

    bool insidePosts = (fabsf(x) < GOAL_HALF_W) && (y > 0.0f) && (y < GOAL_H);
    if (!insidePosts) { g.result = RES_MISS; return; }

    // keeper covers a reach around its dive point
    float reachW = 1.35f;
    float reachH = 2.15f;
    bool covered = (fabsf(x - g.keeperTargetX) < reachW) && (y < reachH);
    g.result = covered ? RES_SAVE : RES_GOAL;
    if (g.result == RES_GOAL) g.scored++;
}

static void UpdateDrawFrame(void)
{
    float dt = GetFrameTime();

    switch (g.phase) {
    case PHASE_AIM: {
        float aimSpd = 0.9f * dt;
        if (IsKeyDown(KEY_LEFT))  g.yaw  -= aimSpd;
        if (IsKeyDown(KEY_RIGHT)) g.yaw  += aimSpd;
        if (IsKeyDown(KEY_UP))    g.pitch += aimSpd;
        if (IsKeyDown(KEY_DOWN))  g.pitch -= aimSpd;
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
            Strike s = {
                .power = 13.0f + g.power01 * 15.0f,
                .yaw = g.yaw, .pitch = g.pitch, .curve = g.curve * 3.2f
            };
            g.ball = BallLaunch((Vector3){0, BALL_R, 0}, s);
            g.keeperTargetX = KeeperGuess();
            g.phase = PHASE_FLY;
        }
    } break;

    case PHASE_FLY: {
        for (int i = 0; i < 4; i++) BallStep(&g.ball, dt / 4.0f);  // sub-steps
        // keeper dives toward its guess
        g.keeperX = Lerp(g.keeperX, g.keeperTargetX, 6.0f * dt);
        g.keeperY = Lerp(g.keeperY, (fabsf(g.keeperTargetX) > 0.6f) ? 1.4f : 0.4f, 5.0f * dt);
        if (g.ball.pos.z >= GOAL_Z) { g.ball.live = false; Judge(); g.phase = PHASE_RESULT; g.resultTimer = 0.0f; }
        else if (g.ball.pos.y < BALL_R && g.ball.vel.y < 0) { // hit turf short
            g.ball.pos.y = BALL_R; g.ball.vel.y *= -0.4f; g.ball.vel = Vector3Scale(g.ball.vel, 0.7f);
            if (Vector3Length(g.ball.vel) < 2.0f) { g.result = RES_MISS; g.phase = PHASE_RESULT; g.resultTimer = 0; }
        }
    } break;

    case PHASE_RESULT: {
        g.resultTimer += dt;
        if (g.resultTimer > 1.6f) {
            g.kick++;
            if (g.kick >= KICKS_TOTAL) { if (IsKeyPressed(KEY_ENTER)) ResetMatch(); }
            else StartKick();
        }
    } break;
    }

    // --- draw ---
    BeginDrawing();
    ClearBackground((Color){ 22, 26, 34, 255 });

    BeginMode3D(g.cam);
        DrawPlane((Vector3){0, 0, 9}, (Vector2){40, 30}, (Color){ 34, 78, 46, 255 });   // pitch
        // penalty spot
        DrawCircle3D((Vector3){0, 0.01f, 0}, 0.25f, (Vector3){1,0,0}, 90.0f, RAYWHITE);
        // goal frame
        Color post = RAYWHITE;
        DrawCube((Vector3){-GOAL_HALF_W, GOAL_H/2, GOAL_Z}, 0.12f, GOAL_H, 0.12f, post);
        DrawCube((Vector3){ GOAL_HALF_W, GOAL_H/2, GOAL_Z}, 0.12f, GOAL_H, 0.12f, post);
        DrawCube((Vector3){0, GOAL_H, GOAL_Z}, GOAL_HALF_W*2, 0.12f, 0.12f, post);
        // net hint
        for (int i = -3; i <= 3; i++)
            DrawLine3D((Vector3){i*GOAL_HALF_W/3, 0, GOAL_Z}, (Vector3){i*GOAL_HALF_W/3, GOAL_H, GOAL_Z}, (Color){200,200,200,90});
        // keeper
        DrawCube((Vector3){g.keeperX, 0.9f + g.keeperY, GOAL_Z - 0.6f}, 0.7f, 1.8f, 0.4f, (Color){ 240, 190, 40, 255 });
        // ball
        DrawSphere(g.ball.pos, BALL_R * 3.0f, RAYWHITE);
        // aim tracer while aiming/charging
        if (g.phase == PHASE_AIM || g.phase == PHASE_CHARGE) {
            Strike s = { .power = 20, .yaw = g.yaw, .pitch = g.pitch, .curve = g.curve*3.2f };
            Ball t = BallLaunch((Vector3){0, BALL_R, 0}, s);
            Vector3 prev = t.pos;
            for (int i = 0; i < 40 && t.pos.z < GOAL_Z + 1; i++) {
                BallStep(&t, 0.04f);
                DrawLine3D(prev, t.pos, (Color){ 255, 90, 90, 160 });
                prev = t.pos;
            }
        }
    EndMode3D();

    // --- HUD ---
    DrawText("WORLD CUP PENALTY", 20, 16, 28, RAYWHITE);
    DrawText(TextFormat("Kick %d / %d     Scored %d", (g.kick < KICKS_TOTAL ? g.kick+1 : KICKS_TOTAL), KICKS_TOTAL, g.scored),
             20, 50, 20, (Color){ 200, 210, 220, 255 });

    if (g.phase == PHASE_AIM)
        DrawText("Arrows: aim   A/D: curl   SPACE: charge power", 20, GetScreenHeight()-34, 20, RAYWHITE);
    if (g.phase == PHASE_CHARGE) {
        DrawText("Release SPACE to strike!", 20, GetScreenHeight()-34, 20, RAYWHITE);
        DrawRectangle(20, GetScreenHeight()-70, 300, 22, (Color){0,0,0,120});
        DrawRectangle(20, GetScreenHeight()-70, (int)(300*g.power01), 22, (Color){ 90, 200, 120, 255 });
    }
    if (g.phase == PHASE_RESULT) {
        const char *msg = g.result==RES_GOAL ? "GOAL!" : g.result==RES_SAVE ? "SAVED!" : "MISS!";
        Color c = g.result==RES_GOAL ? (Color){90,220,120,255} : (Color){235,90,90,255};
        int fs = 60; int w = MeasureText(msg, fs);
        DrawText(msg, GetScreenWidth()/2 - w/2, 90, fs, c);
        if (g.kick >= KICKS_TOTAL-1 && g.resultTimer > 1.6f)
            DrawText("ENTER to play again", GetScreenWidth()/2 - 110, 170, 20, RAYWHITE);
    }

    EndDrawing();
}

int main(void)
{
    const int W = 900, H = 600;
    InitWindow(W, H, "Arcade — World Cup Penalty");

    g.cam = (Camera3D){0};
    g.cam.position = (Vector3){ 0.0f, 2.6f, -6.5f };
    g.cam.target   = (Vector3){ 0.0f, 1.4f, GOAL_Z };
    g.cam.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    g.cam.fovy     = 55.0f;
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
