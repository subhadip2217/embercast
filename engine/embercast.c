/*
 * EMBERCAST — a real-time 3D software engine in a single C file.
 *
 * No GPU. No libc in the WASM build. No textures on disk.
 * DDA raycasting, floor/ceiling, billboards, procgen, hitscan.
 *
 * Coordinate basis (map is X/Y, +Y north):
 *   yaw = 0 faces −Y
 *   forward = (−sin(yaw), −cos(yaw))
 *   right   = ( cos(yaw), −sin(yaw))
 *   A / +turn → +yaw → nose left
 */

#if defined(__wasm__)
#define EC_WASM 1
#else
#define EC_WASM 0
#endif

#if EC_WASM
#define EC_EXPORT __attribute__((visibility("default")))
#else
#define EC_EXPORT
#endif

#include "embercast.h"

typedef unsigned char u8;
typedef unsigned int u32;

#define MAP 32
#define TEX 64
#define TEXN 7
#define MAX_ENT 48
#define MAX_SPARK 96
#define PI 3.14159265f
#define TAU 6.2831853f
#define FOV_TAN 0.6494f /* tan(66°/2) */

#if EC_WASM
void *memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
#endif

/* ---------- math (no libm) ---------- */

static float eclampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}
static float eabsf(float x) { return x < 0.f ? -x : x; }
static float efloor(float x) {
    int i = (int)x;
    return (x < 0.f && (float)i != x) ? (float)(i - 1) : (float)i;
}
static float efract(float x) { return x - efloor(x); }
static float esqrt(float x) { return __builtin_sqrtf(x); }
static float elerp(float a, float b, float t) { return a + (b - a) * t; }

static float esin(float x) {
    /* wrap to [-pi, pi] then Bhaskara-style polynomial */
    x = x - TAU * efloor(x / TAU + 0.5f);
    float ax = eabsf(x);
    float s = (16.f * x * (PI - ax)) / (5.f * PI * PI - 4.f * ax * (PI - ax));
    return s;
}
static float ecos(float x) { return esin(x + PI * 0.5f); }

static unsigned rng;
static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static float rndf(void) { return (rnd() & 0xFFFFFF) / 16777215.f; }

static unsigned phash(int x, int y, unsigned s) {
    unsigned h = (unsigned)(x * 374761393u + y * 668265263u + s * 1274126177u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* ---------- state ---------- */

u32 pixels[EC_WIDTH * EC_HEIGHT];
static float zbuf[EC_WIDTH];
static u8 world[MAP][MAP];
static u32 tex[TEXN][TEX * TEX];
static u32 spr_wraith[TEX * TEX];
static u32 spr_core[TEX * TEX];
static u32 spr_pack[TEX * TEX];

typedef struct {
    u8 alive, kind; /* 1 wraith, 2 core, 3 health, 4 ammo */
    float x, y, hp, bob;
} Ent;

typedef struct {
    u8 alive;
    float x, y, vx, vy, life, r, g, b;
} Spark;

static Ent ents[MAX_ENT];
static Spark sparks[MAX_SPARK];
static int nent;

static float px, py, yaw, pitch, bob;
static float vel, shake;
static int hp, ammo, kills, alive, won;
static int keys;
static int fire_held, fire_edge;
static float analog_f, analog_t, analog_s;
static float look_dx, look_dy;
static float cooldown, flash, hitmark, hurt, time_s;
static unsigned map_seed;
static int core_left;

/* ---------- colour ---------- */

static u32 rgba(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
}
static u32 shade(u32 c, float k) {
    int r = (int)((c & 255) * k);
    int g = (int)(((c >> 8) & 255) * k);
    int b = (int)(((c >> 16) & 255) * k);
    return rgba(r, g, b);
}
static u32 mixc(u32 a, u32 b, float t) {
    if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
    return rgba((int)elerp((float)ar, (float)br, t),
                (int)elerp((float)ag, (float)bg, t),
                (int)elerp((float)ab, (float)bb, t));
}

static const u32 FOG = 0xFF0A0908u;

/* ---------- textures ---------- */

static void gen_textures(void) {
    int i, x, y;
    for (y = 0; y < TEX; y++) for (x = 0; x < TEX; x++) {
        int bx = x / 8, by = y / 4;
        int brick = ((bx + by) & 1);
        int mortar = (y % 4 == 0) || ((x + (by & 1) * 4) % 8 == 0);
        u32 c = mortar ? rgba(28, 22, 18) : (brick ? rgba(92, 48, 32) : rgba(70, 38, 26));
        c = mixc(c, rgba(20, 14, 10), ((phash(x, y, 1) & 31) / 80.f));
        tex[0][y * TEX + x] = c;

        int plate = ((x / 16) ^ (y / 16)) & 1;
        int seam = (x % 16 == 0) || (y % 16 == 0);
        c = seam ? rgba(18, 20, 22) : (plate ? rgba(58, 62, 68) : rgba(42, 46, 52));
        if ((x % 16 == 8 && y % 16 > 6 && y % 16 < 10) ||
            (y % 16 == 8 && x % 16 > 6 && x % 16 < 10))
            c = rgba(160, 70, 28);
        tex[1][y * TEX + x] = c;

        int stripe = ((x + y) / 8) & 1;
        c = stripe ? rgba(28, 24, 18) : rgba(150, 96, 28);
        tex[2][y * TEX + x] = c;

        unsigned n = phash(x / 3, y / 3, 9) & 255;
        c = rgba(40 + (n % 24), 34 + (n % 18), 30);
        if ((phash(x, y, 4) & 255) < 12) c = rgba(110, 40, 18);
        tex[3][y * TEX + x] = c;

        int cell = (x / 8) + (y / 8) * 8;
        int gx = x % 8, gy = y % 8;
        c = rgba(16, 18, 22);
        if (gx == 0 || gy == 0) c = rgba(8, 9, 11);
        if (gx > 2 && gx < 6 && gy > 2 && gy < 6)
            c = (cell & 1) ? rgba(180, 70, 22) : rgba(40, 80, 90);
        tex[4][y * TEX + x] = c;

        float glow = (esin(x * 0.4f) * 0.5f + 0.5f) * (esin(y * 0.3f) * 0.5f + 0.5f);
        c = mixc(rgba(40, 12, 6), rgba(220, 90, 20), glow);
        if ((phash(x, y, 7) & 255) < 20) c = rgba(255, 180, 60);
        tex[5][y * TEX + x] = c;

        c = mixc(rgba(22, 18, 14), rgba(90, 50, 28), ((x ^ y) & 8) ? 0.4f : 0.1f);
        tex[6][y * TEX + x] = c;
    }

    for (i = 0; i < TEX * TEX; i++) {
        spr_wraith[i] = 0;
        spr_core[i] = 0;
        spr_pack[i] = 0;
    }
    for (y = 0; y < TEX; y++) for (x = 0; x < TEX; x++) {
        float u = (x - 32) / 32.f, v = (y - 32) / 32.f;
        float d = esqrt(u * u + v * v * 0.7f);
        if (d < 0.85f) {
            float k = 1.f - d;
            u32 c = mixc(rgba(20, 8, 4), rgba(220, 70, 18), k);
            if (d < 0.2f) c = rgba(255, 210, 120);
            /* eyes */
            if (v < -0.15f && v > -0.4f && (eabsf(u) > 0.18f && eabsf(u) < 0.38f))
                c = rgba(255, 240, 180);
            spr_wraith[y * TEX + x] = c | 0xFF000000u;
        }
        d = esqrt(u * u + v * v);
        if (d < 0.7f) {
            float k = 1.f - d;
            u32 c = mixc(rgba(40, 80, 90), rgba(200, 240, 255), k);
            if (d < 0.22f) c = rgba(255, 255, 255);
            spr_core[y * TEX + x] = c | 0xFF000000u;
        }
        if (eabsf(u) < 0.45f && eabsf(v) < 0.45f) {
            u32 c = rgba(28, 90, 48);
            if (eabsf(u) < 0.12f || eabsf(v) < 0.12f) c = rgba(220, 230, 220);
            spr_pack[y * TEX + x] = c | 0xFF000000u;
        }
    }
}

/* ---------- world ---------- */

static int blocked(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP || y >= MAP) return 1;
    return world[y][x] != 0;
}

static void maze_from(int sx, int sy) {
    int stackx[MAP * MAP], stacky[MAP * MAP], sp = 0;
    stackx[sp] = sx; stacky[sp] = sy; sp++;
    world[sy][sx] = 0;
    while (sp) {
        int x = stackx[sp - 1], y = stacky[sp - 1];
        int dirs[4] = {0, 1, 2, 3};
        int i, carved = 0;
        for (i = 0; i < 4; i++) {
            int j = (int)(rnd() % 4), t = dirs[i];
            dirs[i] = dirs[j]; dirs[j] = t;
        }
        for (i = 0; i < 4; i++) {
            int dx = 0, dy = 0;
            if (dirs[i] == 0) dx = 2;
            if (dirs[i] == 1) dx = -2;
            if (dirs[i] == 2) dy = 2;
            if (dirs[i] == 3) dy = -2;
            int nx = x + dx, ny = y + dy;
            if (nx <= 0 || ny <= 0 || nx >= MAP - 1 || ny >= MAP - 1) continue;
            if (world[ny][nx] == 0) continue;
            world[y + dy / 2][x + dx / 2] = 0;
            world[ny][nx] = 0;
            stackx[sp] = nx; stacky[sp] = ny; sp++;
            carved = 1;
            break;
        }
        if (!carved) sp--;
    }
}

static void paint_walls(void) {
    int x, y;
    for (y = 0; y < MAP; y++) for (x = 0; x < MAP; x++) {
        if (!world[y][x]) continue;
        unsigned h = phash(x, y, map_seed);
        u8 t = (u8)(1 + (h % 5));
        if ((h & 255) < 18) t = 6; /* ember vein */
        if (x == 0 || y == 0 || x == MAP - 1 || y == MAP - 1) t = 2;
        world[y][x] = t;
    }
}

static int open_cell(int x, int y) {
    return x > 0 && y > 0 && x < MAP - 1 && y < MAP - 1 && world[y][x] == 0;
}

static void spawn_ent(u8 kind, float x, float y, float hpv) {
    if (nent >= MAX_ENT) return;
    ents[nent].alive = 1;
    ents[nent].kind = kind;
    ents[nent].x = x;
    ents[nent].y = y;
    ents[nent].hp = hpv;
    ents[nent].bob = rndf() * TAU;
    nent++;
}

static void spark(float x, float y, float vx, float vy, float r, float g, float b, float life) {
    int i;
    for (i = 0; i < MAX_SPARK; i++) if (!sparks[i].alive) {
        sparks[i].alive = 1;
        sparks[i].x = x; sparks[i].y = y;
        sparks[i].vx = vx; sparks[i].vy = vy;
        sparks[i].r = r; sparks[i].g = g; sparks[i].b = b;
        sparks[i].life = life;
        return;
    }
}

static void build_world(unsigned seed) {
    int x, y, i;
    rng = seed ? seed : 0xC0FFEEU;
    if (rng == 0) rng = 1;
    map_seed = rng;
    nent = 0;
    for (i = 0; i < MAX_ENT; i++) ents[i].alive = 0;
    for (i = 0; i < MAX_SPARK; i++) sparks[i].alive = 0;

    for (y = 0; y < MAP; y++) for (x = 0; x < MAP; x++) world[y][x] = 1;
    maze_from(1, 1);

    /* extra loops so it is not a pure tree */
    for (i = 0; i < 28; i++) {
        int cx = 2 + (int)(rnd() % (MAP - 4));
        int cy = 2 + (int)(rnd() % (MAP - 4));
        if (world[cy][cx] &&
            ((open_cell(cx - 1, cy) && open_cell(cx + 1, cy)) ||
             (open_cell(cx, cy - 1) && open_cell(cx, cy + 1))))
            world[cy][cx] = 0;
    }

    /* rooms */
    for (i = 0; i < 4; i++) {
        int rw = 3 + (int)(rnd() % 4), rh = 3 + (int)(rnd() % 4);
        int rx = 2 + (int)(rnd() % (MAP - rw - 4));
        int ry = 2 + (int)(rnd() % (MAP - rh - 4));
        int xx, yy;
        for (yy = ry; yy < ry + rh; yy++)
            for (xx = rx; xx < rx + rw; xx++)
                world[yy][xx] = 0;
    }

    paint_walls();

    /* player spawn: first open near (1,1) */
    px = 1.5f; py = 1.5f;
    for (y = 1; y < MAP - 1; y++) {
        int found = 0;
        for (x = 1; x < MAP - 1; x++) if (open_cell(x, y)) {
            px = x + 0.5f; py = y + 0.5f; found = 1; break;
        }
        if (found) break;
    }
    yaw = 0.f;
    pitch = 0.f;

    /* farthest open cell becomes the core */
    {
        int bestx = (int)px, besty = (int)py, bestd = 0;
        for (y = 1; y < MAP - 1; y++) for (x = 1; x < MAP - 1; x++) {
            if (!open_cell(x, y)) continue;
            int d = (x - (int)px) * (x - (int)px) + (y - (int)py) * (y - (int)py);
            if (d > bestd) { bestd = d; bestx = x; besty = y; }
        }
        spawn_ent(2, bestx + 0.5f, besty + 0.5f, 1.f);
        core_left = 1;
    }

    /* wraiths + packs */
    for (i = 0; i < 9; i++) {
        int tries = 40;
        while (tries--) {
            x = 1 + (int)(rnd() % (MAP - 2));
            y = 1 + (int)(rnd() % (MAP - 2));
            if (!open_cell(x, y)) continue;
            float dx = (x + 0.5f) - px, dy = (y + 0.5f) - py;
            if (dx * dx + dy * dy < 25.f) continue;
            spawn_ent(1, x + 0.5f, y + 0.5f, 40.f);
            break;
        }
    }
    for (i = 0; i < 5; i++) {
        int tries = 20;
        while (tries--) {
            x = 1 + (int)(rnd() % (MAP - 2));
            y = 1 + (int)(rnd() % (MAP - 2));
            if (!open_cell(x, y)) continue;
            spawn_ent((u8)(3 + (rnd() & 1)), x + 0.5f, y + 0.5f, 1.f);
            break;
        }
    }
}

static int los(float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = esqrt(dx * dx + dy * dy);
    int steps, i;
    if (dist < 0.01f) return 1;
    steps = (int)(dist * 6.f) + 1;
    dx /= (float)steps; dy /= (float)steps;
    for (i = 0; i < steps; i++) {
        x0 += dx; y0 += dy;
        if (blocked((int)x0, (int)y0)) return 0;
    }
    return 1;
}

static int try_move(float *x, float *y, float nx, float ny, float rad) {
    int okx = 1, oky = 1;
    float tx = nx, ty = *y;
    int minx = (int)efloor(tx - rad), maxx = (int)efloor(tx + rad);
    int miny = (int)efloor(ty - rad), maxy = (int)efloor(ty + rad);
    int ix, iy;
    for (iy = miny; iy <= maxy; iy++)
        for (ix = minx; ix <= maxx; ix++)
            if (blocked(ix, iy)) okx = 0;
    if (okx) *x = tx;
    tx = *x; ty = ny;
    minx = (int)efloor(tx - rad); maxx = (int)efloor(tx + rad);
    miny = (int)efloor(ty - rad); maxy = (int)efloor(ty + rad);
    for (iy = miny; iy <= maxy; iy++)
        for (ix = minx; ix <= maxx; ix++)
            if (blocked(ix, iy)) oky = 0;
    if (oky) *y = ty;
    return okx || oky;
}

/* ---------- drawing ---------- */

static void putp(int x, int y, u32 c) {
    if ((unsigned)x >= EC_WIDTH || (unsigned)y >= EC_HEIGHT) return;
    pixels[y * EC_WIDTH + x] = c;
}

static void fill_rect(int x0, int y0, int x1, int y1, u32 c) {
    int x, y;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > EC_WIDTH) x1 = EC_WIDTH; if (y1 > EC_HEIGHT) y1 = EC_HEIGHT;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            pixels[y * EC_WIDTH + x] = c;
}

static void draw_world(void) {
    int x, y, i;
    float dirX = -esin(yaw), dirY = -ecos(yaw);
    float planeX = ecos(yaw) * FOV_TAN, planeY = -esin(yaw) * FOV_TAN;
    int horizon = EC_HEIGHT / 2 + (int)(pitch * 140.f);
    float flicker = 0.92f + 0.08f * esin(time_s * 11.f);
    u32 fogcol = mixc(FOG, rgba(40, 16, 8), 0.15f + 0.1f * esin(time_s * 2.f));

    /* ceiling + floor scan */
    for (y = 0; y < EC_HEIGHT; y++) {
        int p = y - horizon;
        if (p == 0) p = 1;
        float row = 0.5f * EC_HEIGHT / (float)(p < 0 ? -p : p);
        if (row < 0.f) row = 0.f;
        float stepX = row * (dirX + planeX - (dirX - planeX)) / (float)EC_WIDTH;
        float stepY = row * (dirY + planeY - (dirY - planeY)) / (float)EC_WIDTH;
        /* left ray is dir - plane */
        float fx = px + row * (dirX - planeX);
        float fy = py + row * (dirY - planeY);
        float fog = eclampf(row / 18.f, 0.f, 1.f);
        for (x = 0; x < EC_WIDTH; x++) {
            int mx = (int)efloor(fx), my = (int)efloor(fy);
            int tx = (int)(efract(fx) * TEX) & (TEX - 1);
            int ty = (int)(efract(fy) * TEX) & (TEX - 1);
            u32 c;
            if (p < 0) {
                c = tex[6][ty * TEX + tx];
                c = shade(c, 0.35f * flicker);
            } else {
                c = tex[3][ty * TEX + tx];
                c = shade(c, 0.55f * flicker);
                if (world[my < 0 ? 0 : (my >= MAP ? MAP - 1 : my)]
                         [mx < 0 ? 0 : (mx >= MAP ? MAP - 1 : mx)] == 0) {
                    /* ember veins on floor near lava walls */
                    if (((mx + my) & 7) == 0) c = mixc(c, rgba(160, 50, 10), 0.25f);
                }
            }
            pixels[y * EC_WIDTH + x] = mixc(c, fogcol, fog);
            fx += stepX; fy += stepY;
        }
    }

    /* walls */
    for (x = 0; x < EC_WIDTH; x++) {
        float cam = 2.f * x / (float)EC_WIDTH - 1.f;
        float rayX = dirX + planeX * cam;
        float rayY = dirY + planeY * cam;
        int mapX = (int)px, mapY = (int)py;
        float ddx = (eabsf(rayX) < 1e-8f) ? 1e30f : eabsf(1.f / rayX);
        float ddy = (eabsf(rayY) < 1e-8f) ? 1e30f : eabsf(1.f / rayY);
        int stepX, stepY, hit = 0, side = 0, guard = 0;
        float sideX, sideY, dist;
        if (rayX < 0) { stepX = -1; sideX = (px - mapX) * ddx; }
        else { stepX = 1; sideX = (mapX + 1.f - px) * ddx; }
        if (rayY < 0) { stepY = -1; sideY = (py - mapY) * ddy; }
        else { stepY = 1; sideY = (mapY + 1.f - py) * ddy; }
        while (!hit && guard++ < 64) {
            if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
            else { sideY += ddy; mapY += stepY; side = 1; }
            if (mapX < 0 || mapY < 0 || mapX >= MAP || mapY >= MAP) { hit = 1; break; }
            if (world[mapY][mapX]) hit = 1;
        }
        if (side == 0) dist = (mapX - px + (1 - stepX) * 0.5f) / (rayX == 0.f ? 1e-6f : rayX);
        else dist = (mapY - py + (1 - stepY) * 0.5f) / (rayY == 0.f ? 1e-6f : rayY);
        if (dist < 0.08f) dist = 0.08f;
        zbuf[x] = dist;

        {
            int lineH = (int)(EC_HEIGHT / dist);
            int y0 = -lineH / 2 + horizon;
            int y1 = lineH / 2 + horizon;
            float wallX = (side == 0) ? (py + dist * rayY) : (px + dist * rayX);
            int tx = (int)(efract(wallX) * TEX);
            if (side == 0 && rayX > 0) tx = TEX - tx - 1;
            if (side == 1 && rayY < 0) tx = TEX - tx - 1;
            if (tx < 0) tx = 0; if (tx >= TEX) tx = TEX - 1;
            int tile = 1;
            if (mapX >= 0 && mapY >= 0 && mapX < MAP && mapY < MAP && world[mapY][mapX])
                tile = world[mapY][mapX];
            int ti = tile - 1;
            if (ti < 0) ti = 0; if (ti >= TEXN) ti = 0;
            float fog = eclampf((dist - 1.f) / 16.f, 0.f, 1.f);
            float lit = (side ? 0.72f : 1.f) * flicker / (1.f + dist * 0.07f);
            int yy;
            if (y0 < 0) y0 = 0;
            if (y1 > EC_HEIGHT) y1 = EC_HEIGHT;
            for (yy = y0; yy < y1; yy++) {
                int d = yy - (-lineH / 2 + horizon);
                int ty = (int)((float)d * TEX / (float)(lineH == 0 ? 1 : lineH)) & (TEX - 1);
                if (ti == 5) ty = (ty + (int)(time_s * 18.f)) & (TEX - 1);
                u32 c = tex[ti][ty * TEX + tx];
                c = shade(c, lit);
                c = mixc(c, fogcol, fog);
                pixels[yy * EC_WIDTH + x] = c;
            }
        }
    }

    /* sprites */
    {
        int order[MAX_ENT];
        float depth[MAX_ENT];
        int n = 0;
        for (i = 0; i < nent; i++) if (ents[i].alive) {
            float dx = ents[i].x - px, dy = ents[i].y - py;
            order[n] = i;
            depth[n] = dx * dx + dy * dy;
            n++;
        }
        /* insertion sort far to near */
        for (i = 1; i < n; i++) {
            int j = i, oi = order[i]; float di = depth[i];
            while (j > 0 && depth[j - 1] < di) {
                depth[j] = depth[j - 1]; order[j] = order[j - 1]; j--;
            }
            depth[j] = di; order[j] = oi;
        }
        for (i = 0; i < n; i++) {
            Ent *e = &ents[order[i]];
            float relX = e->x - px, relY = e->y - py;
            float inv = planeX * dirY - dirX * planeY;
            if (eabsf(inv) < 1e-6f) continue;
            inv = 1.f / inv;
            float tx = inv * (dirY * relX - dirX * relY);
            float ty = inv * (-planeY * relX + planeX * relY);
            if (ty <= 0.12f) continue;
            int sx = (int)((EC_WIDTH / 2.f) * (1.f + tx / ty));
            int sprH = (int)eabsf(EC_HEIGHT / ty);
            int sprW = sprH;
            int y0 = -sprH / 2 + horizon;
            int y1 = sprH / 2 + horizon;
            int x0 = -sprW / 2 + sx;
            int x1 = sprW / 2 + sx;
            const u32 *sheet = spr_wraith;
            if (e->kind == 2) sheet = spr_core;
            if (e->kind == 3 || e->kind == 4) sheet = spr_pack;
            int xx, yy;
            float fog = eclampf((ty - 1.f) / 16.f, 0.f, 1.f);
            if (y0 < 0) y0 = 0; if (y1 > EC_HEIGHT) y1 = EC_HEIGHT;
            if (x0 < 0) x0 = 0; if (x1 > EC_WIDTH) x1 = EC_WIDTH;
            for (xx = x0; xx < x1; xx++) {
                if (ty >= zbuf[xx]) continue;
                int texx = (int)((xx - (-sprW / 2 + sx)) * TEX / (float)(sprW == 0 ? 1 : sprW));
                if (texx < 0) texx = 0; if (texx >= TEX) texx = TEX - 1;
                for (yy = y0; yy < y1; yy++) {
                    int texy = (int)((yy - (-sprH / 2 + horizon)) * TEX / (float)(sprH == 0 ? 1 : sprH));
                    if (texy < 0) texy = 0; if (texy >= TEX) texy = TEX - 1;
                    u32 c = sheet[texy * TEX + texx];
                    if ((c >> 24) < 128) continue;
                    if (e->kind == 2) c = mixc(c, rgba(255, 180, 80), 0.3f + 0.3f * esin(time_s * 4.f));
                    pixels[yy * EC_WIDTH + xx] = mixc(shade(c, flicker), fogcol, fog);
                }
            }
        }
    }

    /* sparks as 2x2 dots */
    for (i = 0; i < MAX_SPARK; i++) if (sparks[i].alive) {
        float relX = sparks[i].x - px, relY = sparks[i].y - py;
        float inv = planeX * dirY - dirX * planeY;
        if (eabsf(inv) < 1e-6f) continue;
        inv = 1.f / inv;
        float tx = inv * (dirY * relX - dirX * relY);
        float ty = inv * (-planeY * relX + planeX * relY);
        if (ty <= 0.1f) continue;
        int sx = (int)((EC_WIDTH / 2.f) * (1.f + tx / ty));
        int sy = horizon;
        if (sx < 0 || sx >= EC_WIDTH || ty > zbuf[sx]) continue;
        u32 c = rgba((int)sparks[i].r, (int)sparks[i].g, (int)sparks[i].b);
        putp(sx, sy, c); putp(sx + 1, sy, c); putp(sx, sy + 1, c);
    }

    /* weapon — geometric viewmodel, never a photo */
    {
        int bobx = (int)(esin(bob) * 6.f);
        int boby = (int)(eabsf(ecos(bob)) * 5.f) + (int)(flash * 8.f);
        int gx = EC_WIDTH / 2 + 70 + bobx;
        int gy = EC_HEIGHT - 8 + boby;
        u32 metal = rgba(48, 50, 56);
        u32 dark = rgba(18, 18, 22);
        u32 ember = rgba(210, 70, 20);
        int k;
        fill_rect(gx - 18, gy - 70, gx + 28, gy - 8, metal);
        fill_rect(gx - 8, gy - 108, gx + 10, gy - 70, dark);
        fill_rect(gx + 8, gy - 92, gx + 42, gy - 78, metal);
        fill_rect(gx - 22, gy - 28, gx + 8, gy, dark);
        if (flash > 0.02f) {
            int f = (int)(flash * 26.f);
            fill_rect(gx + 40, gy - 96 - f / 2, gx + 52 + f, gy - 74 + f / 2, rgba(255, 200, 80));
        }
        /* grip lines */
        for (k = 0; k < 4; k++)
            fill_rect(gx - 16, gy - 60 + k * 10, gx + 24, gy - 59 + k * 10, dark);
        fill_rect(gx + 10, gy - 88, gx + 18, gy - 82, ember);
    }

    /* crosshair */
    {
        u32 ch = hitmark > 0.f ? rgba(220, 230, 220) : rgba(230, 226, 214);
        int cx = EC_WIDTH / 2, cy = horizon;
        fill_rect(cx - 6, cy - 1, cx - 2, cy + 1, ch);
        fill_rect(cx + 2, cy - 1, cx + 6, cy + 1, ch);
        fill_rect(cx - 1, cy - 6, cx + 1, cy - 2, ch);
        fill_rect(cx - 1, cy + 2, cx + 1, cy + 6, ch);
    }

    /* muzzle / damage flashes over the whole buffer */
    if (flash > 0.3f) {
        u32 add = rgba(40, 18, 6);
        for (i = 0; i < EC_WIDTH * EC_HEIGHT; i++)
            pixels[i] = mixc(pixels[i], add, (flash - 0.3f) * 0.4f);
    }
    if (hurt > 0.f) {
        for (i = 0; i < EC_WIDTH * EC_HEIGHT; i++)
            pixels[i] = mixc(pixels[i], rgba(90, 8, 8), hurt * 0.45f);
    }
    if (won) {
        for (i = 0; i < EC_WIDTH * EC_HEIGHT; i++)
            pixels[i] = mixc(pixels[i], rgba(200, 180, 80), 0.18f);
    }
}

/* ---------- combat / sim ---------- */

static void fire_weapon(void) {
    int i, best = -1;
    float bestd = 14.f;
    float dirX = -esin(yaw), dirY = -ecos(yaw);
    if (ammo <= 0 || cooldown > 0.f || !alive) return;
    ammo--;
    cooldown = 0.22f;
    flash = 1.f;
    shake = 0.35f;
    pitch -= 0.03f;
    spark(px + dirX * 0.4f, py + dirY * 0.4f, dirX * 4.f, dirY * 4.f, 255, 180, 60, 0.25f);

    for (i = 0; i < nent; i++) if (ents[i].alive && ents[i].kind == 1) {
        float dx = ents[i].x - px, dy = ents[i].y - py;
        float dist = esqrt(dx * dx + dy * dy);
        if (dist < 0.4f || dist > 14.f) continue;
        float nx = dx / dist, ny = dy / dist;
        float dot = nx * dirX + ny * dirY;
        float ang = 0.12f + 0.02f * dist;
        if (dot < ecos(ang)) continue;
        if (!los(px, py, ents[i].x, ents[i].y)) continue;
        if (dist < bestd) { bestd = dist; best = i; }
    }
    if (best >= 0) {
        Ent *e = &ents[best];
        e->hp -= 22.f;
        hitmark = 0.18f;
        spark(e->x, e->y, (rndf() - 0.5f) * 3.f, (rndf() - 0.5f) * 3.f, 255, 80, 20, 0.4f);
        spark(e->x, e->y, (rndf() - 0.5f) * 3.f, (rndf() - 0.5f) * 3.f, 255, 160, 40, 0.35f);
        if (e->hp <= 0.f) {
            int k;
            e->alive = 0;
            kills++;
            for (k = 0; k < 10; k++)
                spark(e->x, e->y, (rndf() - 0.5f) * 5.f, (rndf() - 0.5f) * 5.f, 220, 60, 10, 0.6f);
        }
    }
}

static void sim_ents(float dt) {
    int i;
    for (i = 0; i < nent; i++) if (ents[i].alive) {
        Ent *e = &ents[i];
        e->bob += dt * 3.f;
        if (e->kind == 1 && alive && !won) {
            float dx = px - e->x, dy = py - e->y;
            float d = esqrt(dx * dx + dy * dy);
            if (d < 0.55f) {
                hp -= (int)(22.f * dt);
                hurt = 0.5f;
                shake = 0.5f;
            } else if (d < 11.f && los(e->x, e->y, px, py)) {
                float sp = 1.35f * dt;
                try_move(&e->x, &e->y, e->x + dx / d * sp, e->y + dy / d * sp, 0.22f);
            } else {
                float a = e->bob * 0.4f;
                try_move(&e->x, &e->y, e->x + esin(a) * 0.4f * dt, e->y + ecos(a) * 0.4f * dt, 0.22f);
            }
        }
        if (e->kind == 2 && alive) {
            float dx = px - e->x, dy = py - e->y;
            if (dx * dx + dy * dy < 0.55f * 0.55f) {
                won = 1;
                e->alive = 0;
                core_left = 0;
            }
        }
        if ((e->kind == 3 || e->kind == 4) && alive) {
            float dx = px - e->x, dy = py - e->y;
            if (dx * dx + dy * dy < 0.4f * 0.4f) {
                if (e->kind == 3) hp += 28;
                else ammo += 14;
                if (hp > 100) hp = 100;
                if (ammo > 80) ammo = 80;
                e->alive = 0;
            }
        }
    }
    if (hp <= 0) { hp = 0; alive = 0; }
}

static void sim_sparks(float dt) {
    int i;
    for (i = 0; i < MAX_SPARK; i++) if (sparks[i].alive) {
        sparks[i].x += sparks[i].vx * dt;
        sparks[i].y += sparks[i].vy * dt;
        sparks[i].life -= dt;
        if (sparks[i].life <= 0.f) sparks[i].alive = 0;
    }
}

/* ---------- public API ---------- */

static void reset_player(void) {
    vel = 0.f;
    shake = 0.f;
    hp = 100;
    ammo = 36;
    kills = 0;
    alive = 1;
    won = 0;
    cooldown = 0.f;
    flash = 0.f;
    hitmark = 0.f;
    hurt = 0.f;
    time_s = 0.f;
    bob = 0.f;
    pitch = 0.f;
    analog_f = analog_t = analog_s = 0.f;
    look_dx = look_dy = 0.f;
    keys = 0;
    fire_held = 0;
    fire_edge = 0;
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_init")))
#endif
void engine_init(unsigned seed) {
    gen_textures();
    build_world(seed);
    reset_player();
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_set_keys")))
#endif
void engine_set_keys(int mask) { keys = mask; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_add_look")))
#endif
void engine_add_look(float dx, float dy) {
    look_dx += dx;
    look_dy += dy;
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_set_analog")))
#endif
void engine_set_analog(float forward, float turn, float strafe) {
    analog_f = eclampf(forward, -1.f, 1.f);
    analog_t = eclampf(turn, -1.f, 1.f);
    analog_s = eclampf(strafe, -1.f, 1.f);
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_set_fire")))
#endif
void engine_set_fire(int down) {
    if (down && !fire_held) fire_edge = 1;
    fire_held = down ? 1 : 0;
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_tick")))
#endif
void engine_tick(float dt) {
    float dirX, dirY, rightX, rightY;
    float fwd, turn, strafe, sp, nx, ny;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.f) dt = 0.f;
    time_s += dt;

    yaw -= look_dx * 0.0024f;
    pitch -= look_dy * 0.0020f;
    look_dx = look_dy = 0.f;
    pitch = eclampf(pitch, -0.7f, 0.7f);

    fwd = analog_f;
    turn = analog_t;
    strafe = analog_s;
    if (keys & EC_KEY_W) fwd += 1.f;
    if (keys & EC_KEY_S) fwd -= 1.f;
    if (keys & EC_KEY_A) turn += 1.f; /* left = +yaw */
    if (keys & EC_KEY_D) turn -= 1.f;
    if (keys & EC_KEY_Q) strafe -= 1.f;
    if (keys & EC_KEY_E) strafe += 1.f;
    fwd = eclampf(fwd, -1.f, 1.f);
    turn = eclampf(turn, -1.f, 1.f);
    strafe = eclampf(strafe, -1.f, 1.f);

    if (alive && !won) {
        yaw += turn * 2.5f * dt;
        dirX = -esin(yaw); dirY = -ecos(yaw);
        rightX = ecos(yaw); rightY = -esin(yaw);
        sp = (keys & EC_KEY_SHIFT) ? 5.2f : 3.25f;
        vel = eabsf(fwd) * sp;
        nx = px + (dirX * fwd + rightX * strafe) * sp * dt;
        ny = py + (dirY * fwd + rightY * strafe) * sp * dt;
        try_move(&px, &py, nx, ny, 0.22f);
        if (eabsf(fwd) > 0.1f || eabsf(strafe) > 0.1f) bob += dt * 10.f * (vel / 3.25f);
        if (fire_edge || (fire_held && cooldown <= 0.f && (keys & EC_KEY_FIRE)))
            fire_weapon();
        fire_edge = 0;
        sim_ents(dt);
    } else {
        vel = 0.f;
        fire_edge = 0;
    }

    sim_sparks(dt);
    if (cooldown > 0.f) cooldown -= dt;
    if (flash > 0.f) flash -= dt * 6.f;
    if (hitmark > 0.f) hitmark -= dt;
    if (hurt > 0.f) hurt -= dt * 2.2f;
    if (shake > 0.f) shake -= dt * 3.f;
    draw_world();
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_pixels")))
#endif
int engine_pixels(void) { return (int)(long)pixels; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_width")))
#endif
int engine_width(void) { return EC_WIDTH; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_height")))
#endif
int engine_height(void) { return EC_HEIGHT; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_x")))
#endif
float engine_x(void) { return px; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_y")))
#endif
float engine_y(void) { return py; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_yaw")))
#endif
float engine_yaw(void) { return yaw; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_pitch")))
#endif
float engine_pitch(void) { return pitch; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_speed")))
#endif
float engine_speed(void) { return vel; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_hp")))
#endif
int engine_hp(void) { return hp; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_ammo")))
#endif
int engine_ammo(void) { return ammo; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_kills")))
#endif
int engine_kills(void) { return kills; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_alive")))
#endif
int engine_alive(void) { return alive; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_won")))
#endif
int engine_won(void) { return won; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_seed")))
#endif
int engine_seed(void) { return (int)map_seed; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_entities")))
#endif
int engine_entities(void) {
    int i, n = 0;
    for (i = 0; i < nent; i++) if (ents[i].alive) n++;
    return n;
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_map_at")))
#endif
int engine_map_at(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP || y >= MAP) return 1;
    return world[y][x];
}

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_map_size")))
#endif
int engine_map_size(void) { return MAP; }

EC_EXPORT
#ifdef __wasm__
__attribute__((export_name("engine_hitmarker")))
#endif
int engine_hitmarker(void) { return hitmark > 0.f ? 1 : 0; }
