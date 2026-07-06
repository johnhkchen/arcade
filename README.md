# arcade

The **hare lane** of the Tortoise-vs-Hare series: a raylib arcade of many small,
good-enough, one-shot micro-games — 3D, real-time, feel-driven — compiled to WASM
and hosted at **arcade.b28.dev**. Breadth + clean deployment is the demonstration.

## The ball-strike core

Three of the sports cartridges (penalty kick, home-run, golf) are the same act —
*strike a ball hard in 3D and track where it lands* — so that lives once in
[`src/ballstrike.{h,c}`](src/ballstrike.h): aim + power + timing + spin → projectile.
Each game is a thin skin over it (its own field, launch input, and scoring).

## Cartridges

| game | status | event hook |
|------|--------|-----------|
| `penalty` — World Cup Penalty Shootout | building | 2026 World Cup (final ~Jul 19) |
| `homerun` — Home Run Contest | planned | MLB Home Run Derby (~mid-July) |
| `golf` — re-skin of the home-run core | planned | Masters |
| `starfox` — roguelite hit-the-targets-in-3D | planned | — |

## Build

```sh
./build.sh penalty      # → dist/penalty/index.html  (needs ~/emsdk + ~/raylib)
```

Toolchain: raylib compiled `PLATFORM=PLATFORM_WEB`, linked by `emcc` per cartridge
(one independent WASM build each — decoupled hare atoms, no shared binary).
