#pragma once
// Constant buffer for shader.hlsl, shared by the app and tests/verify.cpp.
//
// The per-channel sub-pixel shift is identical for every pixel, so the integer tap
// offsets and the interpolation weights are constants of the frame. Computing them
// once here (instead of per pixel in the shader) lets the shader skip taps whose
// weight is exactly zero: a shift along one axis only needs a 1-D filter, which reads
// 4 texels per channel instead of 16.

#include <cmath>
#include <cstdint>

struct ShaderConstants {
    float    wx[3][4];     // R, G, B: horizontal tap weights
    float    wy[3][4];     // R, G, B: vertical tap weights
    int32_t  offs[3][4];   // xy: first tap, zw: Laplacian centre (texels, relative to the output pixel)
    float    k[3][4];      // x: Laplacian compensation strength
    float    params[4];    // texW, texH, unused, unused
    uint32_t mode[4];      // x: B uses R's taps
};
static_assert(sizeof(ShaderConstants) == 224, "cbuffer layout must match shader.hlsl");

struct ShiftSettings {
    float rx, ry, gx, gy, bx, by;   // pixels
    bool  bicubic;
};

// Catmull-Rom weights for the taps at -1, 0, 1, 2 around a fraction t in [0, 1).
inline void CubicWeights(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

// One axis of one channel. s is the centred shift in pixels.
//   sample position  p  = x + 0.5 - n - f      (n = round(s), f = s - n)
//   first tap        ip = floor(p - 0.5) = x - n + floor(-f)
//   fraction         t  = (p - 0.5) - ip = -f - floor(-f)
//   Laplacian centre    = floor(p)       = x - n + floor(0.5 - f)
inline void AxisTaps(float s, bool bicubic, float w[4], int32_t& first, int32_t& lapCentre, float& f) {
    float n = std::nearbyint(s);   // HLSL round(): nearest, ties to even
    f = s - n;
    float d = std::floor(-f);
    float t = -f - d;
    int32_t ip = (int32_t)(-n + d);
    if (bicubic) {
        CubicWeights(t, w);
        first = ip - 1;
    } else {
        w[0] = 1.0f - t;
        w[1] = t;
        w[2] = 0.0f;
        w[3] = 0.0f;
        first = ip;
    }
    lapCentre = (int32_t)(-n + std::floor(0.5f - f));
}

// Laplacian strength that flattens each interpolator's response for a fraction f.
inline float CompensationStrength(bool bicubic, float fx, float fy) {
    return std::fmin((bicubic ? 0.8625f : 1.7216f) * (fx * fx + fy * fy), 0.35f);
}

inline void FillShaderConstants(ShaderConstants& c, const ShiftSettings& s, float texW, float texH) {
    float sx[3] = { s.rx, s.gx, s.bx };
    float sy[3] = { s.ry, s.gy, s.by };

    // Centre the three shifts about zero. A common offset of all channels only moves
    // the whole image by a fraction of a pixel; what cancels the fringe is the relative
    // geometry. Centring keeps the fractional part of every channel small and similar,
    // so no channel loses noticeably more detail to resampling than the others.
    float ctrX = -(std::fmin(sx[0], std::fmin(sx[1], sx[2])) + std::fmax(sx[0], std::fmax(sx[1], sx[2]))) * 0.5f;
    float ctrY = -(std::fmin(sy[0], std::fmin(sy[1], sy[2])) + std::fmax(sy[0], std::fmax(sy[1], sy[2]))) * 0.5f;

    for (int ch = 0; ch < 3; ++ch) {
        float fx, fy;
        AxisTaps(sx[ch] + ctrX, s.bicubic, c.wx[ch], c.offs[ch][0], c.offs[ch][2], fx);
        AxisTaps(sy[ch] + ctrY, s.bicubic, c.wy[ch], c.offs[ch][1], c.offs[ch][3], fy);
        c.k[ch][0] = CompensationStrength(s.bicubic, fx, fy);
        c.k[ch][1] = c.k[ch][2] = c.k[ch][3] = 0.0f;
    }

    c.params[0] = texW;
    c.params[1] = texH;
    c.params[2] = c.params[3] = 0.0f;

    bool sameRB = c.k[0][0] == c.k[2][0];
    for (int i = 0; i < 4; ++i)
        sameRB = sameRB && c.wx[0][i] == c.wx[2][i] && c.wy[0][i] == c.wy[2][i] && c.offs[0][i] == c.offs[2][i];
    c.mode[0] = sameRB;
    c.mode[1] = c.mode[2] = c.mode[3] = 0;
}
