#include "jumbotron.h"
#include <math.h>

static int FitFont(const char *s, int size, int maxw)
{
    while (size > 6 && MeasureText(s, size) > maxw) size--;
    return size;
}
static Color Dim(Color c, float f)
{
    return (Color){ (unsigned char)(c.r*f), (unsigned char)(c.g*f), (unsigned char)(c.b*f), 255 };
}

void DrawJumbotron(Rectangle r, BoardData d, float t)
{
    if (r.width < 30 || r.height < 14) return;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    // ---- era palette ----
    Color bg, ink, accent;
    bool mono = (d.era <= ERA_90S);
    switch (d.era) {
    case ERA_70S: bg = (Color){12,7,0,255};  ink = (Color){255,176,0,255};   accent = ink; break;              // amber bulbs
    case ERA_90S: bg = (Color){4,11,6,255};  ink = (Color){120,240,130,255}; accent = (Color){240,220,60,255}; break; // green CRT
    case ERA_00S: bg = (Color){6,8,14,255};  ink = (Color){235,240,245,255}; accent = (Color){90,160,255,255}; break; // LED matrix
    default:      bg = (Color){10,14,22,255}; ink = (Color){240,244,250,255}; accent = (Color){90,200,255,255}; break; // modern
    }

    DrawRectangleRec(r, bg);
    if (d.era == ERA_MODERN)
        DrawRectangleGradientV((int)r.x, (int)r.y, (int)r.width, (int)r.height, (Color){18,26,44,255}, (Color){6,8,14,255});

    // team color bars (only the color eras show real kit colors)
    if (d.era >= ERA_00S) {
        float bw = r.width*0.035f;
        DrawRectangle((int)r.x, (int)r.y, (int)bw, (int)r.height, d.homeA);
        DrawRectangle((int)(r.x+bw), (int)r.y, (int)(bw*0.5f), (int)r.height, d.homeB);
        DrawRectangle((int)(r.x+r.width-bw), (int)r.y, (int)bw, (int)r.height, d.awayA);
        DrawRectangle((int)(r.x+r.width-bw*1.5f), (int)r.y, (int)(bw*0.5f), (int)r.height, d.awayB);
    }

    // title
    if (d.title) {
        int ts = FitFont(d.title, (int)(r.height*0.16f), (int)(r.width*0.9f));
        DrawText(d.title, (int)(r.x + r.width/2 - MeasureText(d.title, ts)/2.0f), (int)(r.y + r.height*0.07f),
                 ts, mono ? accent : (Color){130,150,185,255});
    }

    // big center line (a hair of flicker on the old boards)
    if (d.big) {
        int bs = FitFont(d.big, (int)(r.height*0.34f), (int)(r.width*0.88f));
        int bx = (int)(r.x + r.width/2 - MeasureText(d.big, bs)/2.0f), by = (int)(r.y + r.height*0.34f);
        Color bc = mono ? ink : d.bigColor;
        if (d.era == ERA_70S) DrawText(d.big, bx+2, by+2, bs, Dim(ink, 0.32f));   // bulb glow/ghost
        if (mono && (int)(t*8) % 24 == 0) bc = Dim(bc, 0.7f);                     // faint flicker
        DrawText(d.big, bx, by, bs, bc);
    }

    // kick dots
    if (d.dots > 0) {
        float dr = r.height*0.05f;
        float gap = fminf(r.width*0.85f/d.dots, dr*3.4f);
        float sx = r.x + r.width/2 - (d.dots-1)*gap/2.0f, dy = r.y + r.height*0.82f;
        for (int i = 0; i < d.dots; i++) {
            float x = sx + i*gap;
            Color dc;
            if (d.dotState[i] == 1)      dc = mono ? ink            : (Color){90,220,120,255};
            else if (d.dotState[i] == 2) dc = mono ? Dim(ink,0.45f) : (Color){235,90,90,255};
            else                         dc = Dim(ink, 0.22f);
            DrawCircle((int)x, (int)dy, dr, dc);
            if (d.dotState[i] == 0) DrawCircleLines((int)x, (int)dy, dr, Dim(ink, 0.4f));
        }
    }

    // corner year label
    if (d.year) {
        int ys = FitFont(d.year, (int)(r.height*0.12f), (int)(r.width*0.25f));
        DrawText(d.year, (int)(r.x + r.width - MeasureText(d.year, ys) - r.height*0.06f),
                 (int)(r.y + r.height*0.07f), ys, mono ? accent : (Color){110,120,140,255});
    }

    // ---- era overlays ----
    if (d.era == ERA_70S || d.era == ERA_90S)                        // CRT scanlines
        for (float yy = r.y; yy < r.y + r.height; yy += 3.0f)
            DrawRectangle((int)r.x, (int)yy, (int)r.width, 1, (Color){0,0,0,70});
    if (d.era == ERA_00S) {                                          // LED pixel grid
        float step = fmaxf(4.0f, r.height*0.05f);
        for (float yy = r.y; yy < r.y + r.height; yy += step) DrawRectangle((int)r.x, (int)yy, (int)r.width, 1, (Color){0,0,0,85});
        for (float xx = r.x; xx < r.x + r.width; xx += step) DrawRectangle((int)xx, (int)r.y, 1, (int)r.height, (Color){0,0,0,85});
    }
    DrawRectangleLinesEx(r, 2.0f, (Color){0,0,0,120});

    EndScissorMode();
}
