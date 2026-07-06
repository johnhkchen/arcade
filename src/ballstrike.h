// ballstrike.h — the shared "strike a ball hard in 3D" core.
//
// Three of the arcade's sports microgames (penalty kick, home-run, golf) are the
// same act: aim + power + timing + spin -> a 3D projectile -> where does it land.
// This is that core, deliberately tiny and skin-agnostic. Each game supplies its
// own launch input, field, and scoring; the flight physics live here once.
#ifndef BALLSTRIKE_H
#define BALLSTRIKE_H

#include "raylib.h"

typedef struct {
    Vector3 pos;   // current position (m)
    Vector3 vel;   // current velocity (m/s)
    Vector3 spin;  // spin axis * rate, drives Magnus curve
    bool live;     // false once it's done flying (game decides when)
} Ball;

// A strike, in the ball's local launch frame: +z is "downfield" (toward the goal
// for penalty, toward the outfield for home-run). yaw/pitch in radians.
typedef struct {
    float power;   // launch speed (m/s)
    float yaw;     // aim left(-)/right(+)
    float pitch;   // aim down(-)/up(+)
    float curve;   // sidespin: negative curls left, positive curls right
} Strike;

// Launch a ball from `origin` with the given strike.
Ball BallLaunch(Vector3 origin, Strike s);

// Advance one step: gravity + light drag + Magnus curve from spin.
void BallStep(Ball *b, float dt);

#endif // BALLSTRIKE_H
