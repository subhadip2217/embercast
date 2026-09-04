#define _POSIX_C_SOURCE 200809L
/*
 * Native host for EMBERCAST.
 *
 *   ./embercast            — 180-frame benchmark
 *   ./embercast --ppm out.ppm
 *   ./embercast --selftest
 */

#include "embercast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned int pixels[EC_WIDTH * EC_HEIGHT];

static int dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", EC_WIDTH, EC_HEIGHT);
    for (i = 0; i < EC_WIDTH * EC_HEIGHT; i++) {
        unsigned c = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)(c & 255),
            (unsigned char)((c >> 8) & 255),
            (unsigned char)((c >> 16) & 255)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int bench(void) {
    int i, lit = 0;
    double t0, t1;
    engine_init(0xE11BE5u);
    engine_set_keys(EC_KEY_W);
    t0 = now_s();
    for (i = 0; i < 180; i++) engine_tick(1.f / 60.f);
    t1 = now_s();
    for (i = 0; i < EC_WIDTH * EC_HEIGHT; i++) {
        unsigned c = pixels[i] & 0x00FFFFFFu;
        if (c) lit++;
    }
    printf("EMBERCAST native bench\n");
    printf("  frames     180\n");
    printf("  resolution %dx%d\n", EC_WIDTH, EC_HEIGHT);
    printf("  elapsed    %.3f s\n", t1 - t0);
    printf("  fps        %.1f\n", 180.0 / (t1 - t0));
    printf("  lit px     %d / %d\n", lit, EC_WIDTH * EC_HEIGHT);
    printf("  pos        %.2f, %.2f  yaw %.2f\n", engine_x(), engine_y(), engine_yaw());
    return lit > 1000 ? 0 : 2;
}

static int selftest(void) {
    float y0, y1;
    engine_init(7);
    if (engine_width() != EC_WIDTH || engine_height() != EC_HEIGHT) {
        fprintf(stderr, "size mismatch\n");
        return 1;
    }
    if (engine_hp() != 100 || !engine_alive()) {
        fprintf(stderr, "spawn state bad\n");
        return 1;
    }
    y0 = engine_yaw();
    engine_set_keys(EC_KEY_A);
    engine_tick(0.5f);
    y1 = engine_yaw();
    if (!(y1 > y0 + 0.2f)) {
        fprintf(stderr, "A did not increase yaw (left). y0=%.3f y1=%.3f\n", y0, y1);
        return 1;
    }
    engine_set_keys(0);
    engine_set_keys(EC_KEY_W);
    engine_tick(0.4f);
    if (engine_speed() <= 0.1f) {
        fprintf(stderr, "W did not produce speed\n");
        return 1;
    }
    printf("selftest ok  yaw-left=%.3f speed=%.2f entities=%d\n",
           y1 - y0, engine_speed(), engine_entities());
    return 0;
}

int main(int argc, char **argv) {
    int i;
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc >= 3 && strcmp(argv[1], "--ppm") == 0) {
        engine_init(0xE11BE5u);
        engine_set_keys(EC_KEY_W);
        for (i = 0; i < 45; i++) engine_tick(1.f / 60.f);
        return dump_ppm(argv[2]);
    }
    return bench();
}
