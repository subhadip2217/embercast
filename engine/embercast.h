#ifndef EMBERCAST_H
#define EMBERCAST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Framebuffer is 480x270 RGBA8888 (R in the low byte). */
#define EC_WIDTH  480
#define EC_HEIGHT 270

void engine_init(unsigned seed);
void engine_set_keys(int mask);
void engine_add_look(float dx, float dy);
void engine_set_analog(float forward, float turn, float strafe);
void engine_set_fire(int down);
void engine_tick(float dt);

int engine_pixels(void);
int engine_width(void);
int engine_height(void);
float engine_x(void);
float engine_y(void);
float engine_yaw(void);
float engine_pitch(void);
float engine_speed(void);
int engine_hp(void);
int engine_ammo(void);
int engine_kills(void);
int engine_alive(void);
int engine_won(void);
int engine_seed(void);
int engine_entities(void);
int engine_map_at(int x, int y);
int engine_map_size(void);
int engine_hitmarker(void);

/* Native hosts may dump the framebuffer directly. WASM uses engine_pixels(). */
extern unsigned int pixels[EC_WIDTH * EC_HEIGHT];

enum {
    EC_KEY_W = 1 << 0,
    EC_KEY_S = 1 << 1,
    EC_KEY_A = 1 << 2, /* turn left  (+yaw) */
    EC_KEY_D = 1 << 3, /* turn right (-yaw) */
    EC_KEY_Q = 1 << 4, /* strafe left */
    EC_KEY_E = 1 << 5, /* strafe right */
    EC_KEY_SHIFT = 1 << 6,
    EC_KEY_FIRE = 1 << 7
};

#ifdef __cplusplus
}
#endif

#endif
