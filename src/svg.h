// svg.h — a tiny SVG rasterizer for the arcade's baked-texture art.
//
// The arcade builds its playing-piece art (soccer-ball skin, keeper faces, kit
// crests…) as small hand-written SVG strings and bakes them to textures once at
// boot. This is the renderer: a deliberately minimal, dependency-free subset of
// SVG — rect / circle / ellipse / polygon / path (M,L,H,V,C,Q,Z), solid `fill`
// and `stroke` — rasterized with supersampled anti-aliasing into a raylib Image.
// It is skin-agnostic like ballstrike/jumbotron: any cartridge can reuse it.
//
// Authoring: put a `viewBox="minx miny w h"` on the root <svg> (defaults to
// 0 0 100 100). Coordinates are in that space; the output pixel size is chosen
// by the caller, so the same markup renders crisp at any resolution.
#ifndef SVG_H
#define SVG_H

#include "raylib.h"

// Rasterize an SVG document string into an RGBA image `outW`x`outH` pixels.
// Areas no shape covers stay fully transparent. Caller UnloadImage()s the result.
Image SvgRasterize(const char *svg, int outW, int outH);

// Convenience: rasterize and upload straight to a GPU texture (bilinear-filtered).
Texture2D SvgTexture(const char *svg, int outW, int outH);

#endif // SVG_H
