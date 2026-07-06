// jumbotron.h — a reusable stadium scoreboard renderer for the arcade's sports games.
//
// Draws live match data into an on-screen rectangle in a chosen PERIOD-ACCURATE
// style (amber bulb board, green CRT, RGB LED matrix, modern LED wall). Pure 2D:
// the caller projects the physical board's screen rect and hands over the data, so
// any cartridge (penalty, home-run, ...) can reuse it.
#ifndef JUMBOTRON_H
#define JUMBOTRON_H

#include "raylib.h"

typedef enum { ERA_70S, ERA_90S, ERA_00S, ERA_MODERN, ERA_COUNT } BoardEra;

typedef struct {
    BoardEra    era;
    const char *year;              // small corner label, e.g. "1978"
    const char *title;             // top line, e.g. "ROUND 2 / 3"
    const char *sub;               // subtitle, e.g. "BEAT 4"
    const char *big;               // center line, e.g. "GOAL!" / "YOU 3"
    Color       bigColor;          // used by color eras (mono eras override)
    Color       homeA, homeB, awayA, awayB;
    int         dots;              // number of kick slots (<= 12)
    int         dotState[12];      // 0 pending, 1 scored, 2 kept-out
} BoardData;

// Render the board into `rect` (screen pixels). `t` is a time value for flicker.
void DrawJumbotron(Rectangle rect, BoardData d, float t);

#endif // JUMBOTRON_H
