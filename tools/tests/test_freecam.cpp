// g++ -std=c++17 -I platform/mobile tools/tests/test_freecam.cpp -o /tmp/test_freecam && /tmp/test_freecam
#include <stdio.h>
#include <math.h>
#include "freecam.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void apply(const float m[16], const float v[3], float w, float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * w;
}

int main() {
    // An arbitrary game camera: rotated 30 deg about Y, then 20 deg about X, at some position.
    const float ay = 0.52f, ax = 0.35f;
    float Ry[9] = {cosf(ay), 0, sinf(ay), 0, 1, 0, -sinf(ay), 0, cosf(ay)};
    float Rx[9] = {1, 0, 0, 0, cosf(ax), -sinf(ax), 0, sinf(ax), cosf(ax)};
    float Rg[9];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        Rg[r * 3 + c] = Rx[r * 3 + 0] * Ry[0 * 3 + c] + Rx[r * 3 + 1] * Ry[1 * 3 + c] + Rx[r * 3 + 2] * Ry[2 * 3 + c];
    float view[12] = {Rg[0], Rg[1], Rg[2], 123.0f, Rg[3], Rg[4], Rg[5], -456.0f, Rg[6], Rg[7], Rg[8], 7890.0f};

    freecam::State s;
    freecam::begin(s, view);
    float m[16];
    freecam::eyeView(s, view, m);

    // 1. At start the free camera IS the game camera: identity.
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float e = (r == c) ? 1.0f : 0.0f;
        CHECK(fabsf(m[r * 4 + c] - e) < 1e-3f, "identity [%d][%d] = %f", r, c, m[r * 4 + c]);
    }

    // 2. Moving forward 100 units brings a point straight ahead 100 units closer.
    {
        freecam::State t = s;
        // kMoveSpeed units/s * dt = 100  ->  dt = 100 / kMoveSpeed
        freecam::step(t, 100.0f / freecam::kMoveSpeed, 0, 0, 1.0f, 0, 0, false);
        freecam::eyeView(t, view, m);
        const float ahead[3] = {0, 0, -500};
        float o[3];
        apply(m, ahead, 1.0f, o);
        CHECK(fabsf(o[0]) < 0.5f && fabsf(o[1]) < 0.5f && fabsf(o[2] + 400.0f) < 0.5f,
              "forward: %f %f %f (want 0 0 -400)", o[0], o[1], o[2]);
    }

    // 3. The camera is anchored in the world: if the GAME camera moves, the
    //    free camera view of a world point must not change.
    {
        float wp[3] = {1000, 200, -3000};  // world point
        auto viewOf = [&](const float v[12], const float p[3], float out[3]) {
            for (int r = 0; r < 3; r++) out[r] = v[r * 4] * p[0] + v[r * 4 + 1] * p[1] + v[r * 4 + 2] * p[2] + v[r * 4 + 3];
        };
        freecam::State t = s;
        freecam::step(t, 0.3f, 0.2f, 0.1f, 0.5f, 0.4f, -0.2f, false);
        float v2[12];
        for (int i = 0; i < 12; i++) v2[i] = view[i];
        v2[3] += 5000.0f; v2[7] -= 2000.0f;  // the game camera moved
        float g1[3], g2[3], o1[3], o2[3];
        viewOf(view, wp, g1);
        viewOf(v2, wp, g2);
        float m1[16], m2[16];
        freecam::eyeView(t, view, m1);
        freecam::eyeView(t, v2, m2);
        apply(m1, g1, 1.0f, o1);
        apply(m2, g2, 1.0f, o2);
        for (int i = 0; i < 3; i++) CHECK(fabsf(o1[i] - o2[i]) < 0.05f, "anchor axis %d: %f vs %f", i, o1[i], o2[i]);
    }

    // 4. Turning right keeps the rotation orthonormal over many steps.
    {
        freecam::State t = s;
        for (int i = 0; i < 5000; i++) freecam::step(t, 1.0f / 60.0f, 0, 0, 0, 0.7f, 0.3f, false);
        float c0 = t.R[0] * t.R[0] + t.R[3] * t.R[3] + t.R[6] * t.R[6];
        float c01 = t.R[0] * t.R[1] + t.R[3] * t.R[4] + t.R[6] * t.R[7];
        CHECK(fabsf(c0 - 1.0f) < 1e-4f && fabsf(c01) < 1e-4f, "orthonormal drift %f %f", c0, c01);
    }

    // 5. Turning right: the old forward direction ends up on the left of the view.
    {
        freecam::State t = s;
        freecam::step(t, 0.2f, 0, 0, 0, 1.0f, 0, false);
        freecam::eyeView(t, view, m);
        const float ahead[3] = {0, 0, -100};
        float o[3];
        apply(m, ahead, 0.0f, o);
        CHECK(o[0] < -1.0f, "turn right should move old forward to the left, x=%f", o[0]);
    }

    // 6. Projection maps the near plane to -1 and far to +1.
    {
        float P[16];
        freecam::projection(60.0f, 16.0f / 9.0f, P);
        float zn = (P[10] * -freecam::kNearZ + P[11]) / (-P[14] * -freecam::kNearZ * -1.0f);
        (void)zn;
        float cn[2] = {P[10] * -freecam::kNearZ + P[11], P[14] * -freecam::kNearZ};
        float cf[2] = {P[10] * -freecam::kFarZ + P[11], P[14] * -freecam::kFarZ};
        CHECK(fabsf(cn[0] / cn[1] + 1.0f) < 1e-3f, "near ndc %f", cn[0] / cn[1]);
        CHECK(fabsf(cf[0] / cf[1] - 1.0f) < 1e-3f, "far ndc %f", cf[0] / cf[1]);
    }

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
