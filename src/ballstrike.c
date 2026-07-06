#include "ballstrike.h"
#include "raymath.h"
#include <math.h>

#define BS_GRAVITY 9.81f
#define BS_DRAG    0.04f   // per-second velocity bleed (crude air resistance)
#define BS_MAGNUS  0.85f   // strength of spin-induced curve

Ball BallLaunch(Vector3 origin, Strike s)
{
    Ball b = (Ball){0};
    b.pos = origin;

    float cp = cosf(s.pitch);
    Vector3 dir = (Vector3){ sinf(s.yaw) * cp, sinf(s.pitch), cosf(s.yaw) * cp };
    b.vel = Vector3Scale(Vector3Normalize(dir), s.power);

    // Sidespin about the vertical axis -> horizontal Magnus curve.
    b.spin = (Vector3){ 0.0f, s.curve, 0.0f };
    b.live = true;
    return b;
}

void BallStep(Ball *b, float dt)
{
    if (!b->live) return;

    // gravity
    b->vel.y -= BS_GRAVITY * dt;
    // drag (opposes motion)
    b->vel = Vector3Add(b->vel, Vector3Scale(b->vel, -BS_DRAG * dt));
    // Magnus: a ~ spin x vel
    Vector3 magnus = Vector3CrossProduct(b->spin, b->vel);
    b->vel = Vector3Add(b->vel, Vector3Scale(magnus, BS_MAGNUS * dt));
    // integrate position
    b->pos = Vector3Add(b->pos, Vector3Scale(b->vel, dt));
}
