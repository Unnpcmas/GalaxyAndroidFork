// Free camera maths (header only, no Android or GL dependencies so it can be
// unit tested on a desktop: tools/tests/test_freecam.cpp).
//
// The camera lives in WORLD space, so it stays where the player put it while
// the game's own camera follows Mario around.  Each frame the renderer is given
// the matrix that takes the game's view space to the free camera's view space
// (the same hook the VR eye uses), so nothing in the game itself changes.
//
// Conventions: matrices are row-major, vectors are columns.  The game's view
// is the recorded "world -> view" 3x4 (rows).  View space is OpenGL-like
// (looks down -Z, +Y up).
#pragma once

#include <math.h>

namespace freecam {

struct State {
    bool active = false;
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // camera -> world, row-major (columns = right, up, back)
    float p[3] = {0, 0, 0};                    // camera position in world space
};

// Tuning (game units; Mario runs at roughly 1000 units per second).
constexpr float kMoveSpeed = 1500.0f;  // units per second at full stick
constexpr float kFastFactor = 4.0f;    // while the fast button is held
constexpr float kLookRate = 1.8f;      // radians per second at full stick
constexpr float kNearZ = 30.0f;
constexpr float kFarZ = 800000.0f;

inline void orthonormalize(float R[9]) {
    // Columns c0 (right), c1 (up), c2 (back).
    float c0[3] = {R[0], R[3], R[6]};
    float c1[3] = {R[1], R[4], R[7]};
    float n0 = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    if (n0 < 1e-6f) return;
    for (int i = 0; i < 3; i++) c0[i] /= n0;
    float d = c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2];
    for (int i = 0; i < 3; i++) c1[i] -= d * c0[i];
    float n1 = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    if (n1 < 1e-6f) return;
    for (int i = 0; i < 3; i++) c1[i] /= n1;
    float c2[3] = {c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]};
    for (int i = 0; i < 3; i++) {
        R[i * 3 + 0] = c0[i];
        R[i * 3 + 1] = c1[i];
        R[i * 3 + 2] = c2[i];
    }
}

// Starts at exactly the game's current view: `view` is the recorded
// world -> view 3x4 (12 floats, rows).
inline void begin(State& s, const float view[12]) {
    // Rg = upper 3x3 of the view, t = its translation.  World -> view is
    // v = Rg * w + t, so camera -> world is R = Rg^T and p = -Rg^T * t.
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) s.R[r * 3 + c] = view[c * 4 + r];
    const float t[3] = {view[3], view[7], view[11]};
    for (int r = 0; r < 3; r++) s.p[r] = -(s.R[r * 3 + 0] * t[0] + s.R[r * 3 + 1] * t[1] + s.R[r * 3 + 2] * t[2]);
    orthonormalize(s.R);
    s.active = true;
}

// move: x = right, y = up, z = forward (all -1..1).  look: x = turn right,
// y = look up (-1..1).  fast multiplies the speed.
inline void step(State& s, float dt, float moveX, float moveY, float moveZ,
                 float lookX, float lookY, bool fast) {
    if (!(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;

    // Turn about the camera's own axes: yaw about up (turning right is a
    // negative turn about +Y), pitch about right.
    const float yaw = -lookX * kLookRate * dt;
    const float pitch = lookY * kLookRate * dt;
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float Ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    const float Rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
    float T[9], N[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            T[r * 3 + c] = Ry[r * 3 + 0] * Rx[0 * 3 + c] + Ry[r * 3 + 1] * Rx[1 * 3 + c] + Ry[r * 3 + 2] * Rx[2 * 3 + c];
        }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            N[r * 3 + c] = s.R[r * 3 + 0] * T[0 * 3 + c] + s.R[r * 3 + 1] * T[1 * 3 + c] + s.R[r * 3 + 2] * T[2 * 3 + c];
        }
    for (int i = 0; i < 9; i++) s.R[i] = N[i];
    orthonormalize(s.R);

    // Move in camera space (forward is -Z), then into the world.
    const float speed = kMoveSpeed * (fast ? kFastFactor : 1.0f) * dt;
    const float m[3] = {moveX * speed, moveY * speed, -moveZ * speed};
    for (int r = 0; r < 3; r++) s.p[r] += s.R[r * 3 + 0] * m[0] + s.R[r * 3 + 1] * m[1] + s.R[r * 3 + 2] * m[2];
}

// out = matrix taking GAME view space to the free camera's view space
// (4x4 row-major): Vfree * inverse(Vgame).
inline void eyeView(const State& s, const float gameView[12], float out[16]) {
    // inverse(Vgame): camera -> world of the game camera.
    float Rg[9], tg[3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) Rg[r * 3 + c] = gameView[r * 4 + c];
    for (int r = 0; r < 3; r++) tg[r] = gameView[r * 4 + 3];
    // Wg = [Rg^T | -Rg^T tg]
    float Wr[9], Wt[3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) Wr[r * 3 + c] = Rg[c * 3 + r];
    for (int r = 0; r < 3; r++) Wt[r] = -(Wr[r * 3 + 0] * tg[0] + Wr[r * 3 + 1] * tg[1] + Wr[r * 3 + 2] * tg[2]);
    // Vfree = [R^T | -R^T p]
    float Fr[9], Ft[3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) Fr[r * 3 + c] = s.R[c * 3 + r];
    for (int r = 0; r < 3; r++) Ft[r] = -(Fr[r * 3 + 0] * s.p[0] + Fr[r * 3 + 1] * s.p[1] + Fr[r * 3 + 2] * s.p[2]);
    // out = Vfree * Wg
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            out[r * 4 + c] = Fr[r * 3 + 0] * Wr[0 * 3 + c] + Fr[r * 3 + 1] * Wr[1 * 3 + c] + Fr[r * 3 + 2] * Wr[2 * 3 + c];
        out[r * 4 + 3] = Fr[r * 3 + 0] * Wt[0] + Fr[r * 3 + 1] * Wt[1] + Fr[r * 3 + 2] * Wt[2] + Ft[r];
    }
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

// The free camera's position expressed in the game's view space (lights the
// game attaches to its camera are lit from here).
inline void positionInGameView(const State& s, const float gameView[12], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = gameView[r * 4 + 0] * s.p[0] + gameView[r * 4 + 1] * s.p[1] + gameView[r * 4 + 2] * s.p[2] + gameView[r * 4 + 3];
}

// OpenGL perspective projection (row-major), view space -> clip space.
inline void projection(float fovyDeg, float aspect, float out[16], float nearZ = kNearZ, float farZ = kFarZ) {
    if (fovyDeg > 170.0f) fovyDeg = 170.0f;
    if (!(fovyDeg > 1.0f)) fovyDeg = 45.0f;
    if (!(aspect > 0.1f)) aspect = 16.0f / 9.0f;
    const float f = 1.0f / tanf(fovyDeg * 0.5f * 3.14159265358979f / 180.0f);
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (farZ + nearZ) / (nearZ - farZ);
    out[11] = 2.0f * farZ * nearZ / (nearZ - farZ);
    out[14] = -1.0f;
}

}  // namespace freecam
