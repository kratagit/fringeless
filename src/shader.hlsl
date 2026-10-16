// Fringeless correction shader.
//
// Each colour channel is resampled at a constant sub-pixel offset, which moves it
// relative to the other two and cancels the colour fringe a non-stripe subpixel layout
// puts on ClearType text. The resample is an interpolator (bilinear or Catmull-Rom)
// followed by a small Laplacian that restores the contrast the interpolation loses.
//
// Performance notes:
//  * the shift is uniform, so tap offsets and filter weights come precomputed from
//    the CPU (src/shift_constants.h) instead of being derived per pixel;
//  * taps whose weight is exactly zero are skipped with uniform branches, so a
//    shift along one axis costs a 1-D filter (4 reads) instead of a 4x4 one (16);
//  * every Load already returns all three channels, so when blue uses the same
//    taps as red both come from one set of reads.

Texture2D tex : register(t0);
Texture2D incoming : register(t1);   // diffmain only
cbuffer CB : register(b0) {
    float4 wx[3];       // R, G, B: horizontal tap weights
    float4 wy[3];       // R, G, B: vertical tap weights
    int4   offs[3];     // xy: first tap, zw: Laplacian centre (relative to the output pixel)
    float4 k[3];        // x: Laplacian compensation strength
    float4 params;      // x: texW, y: texH
    uint4  mode;        // x: B uses R's taps
};

struct VSOut { float4 pos : SV_Position; };

VSOut vsmain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 LoadRGB(int2 texel) {
    int2 c = int2(clamp(texel.x, 0, (int)params.x - 1), clamp(texel.y, 0, (int)params.y - 1));
    return tex.Load(int3(c, 0)).rgb;
}

// Separable interpolation of one channel's taps: sum wy[j] * sum wx[i] * T(first + (i, j)).
float3 Interpolate(int2 px, float4 hx, float4 hy, int2 first) {
    float3 result = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [branch] if (hy[j] != 0) {
            float3 rowsum = 0;
            [unroll] for (int i = 0; i < 4; ++i) {
                [branch] if (hx[i] != 0)
                    rowsum += hx[i] * LoadRGB(px + first + int2(i, j));
            }
            result += hy[j] * rowsum;
        }
    }
    return result;
}

// 5-tap Laplacian, used to flatten the interpolator's frequency response.
float3 Laplacian(int2 p) {
    float3 c = LoadRGB(p);
    return (LoadRGB(p + int2(-1, 0)) + LoadRGB(p + int2(1, 0))
          + LoadRGB(p + int2(0, -1)) + LoadRGB(p + int2(0, 1))) - 4.0 * c;
}

// Change detection: survives (and is counted by an occlusion query) only where the
// incoming desktop frame differs from the copy the overlay was rendered from.
float4 diffmain(VSOut i) : SV_Target {
    int3 p = int3(floor(i.pos.xy), 0);
    if (all(tex.Load(p) == incoming.Load(p))) discard;
    return 0;
}

float4 psmain(VSOut i) : SV_Target {
    int2 px = int2(floor(i.pos.xy));
    float r, g, b;

    float3 sr = Interpolate(px, wx[0], wy[0], offs[0].xy);
    float3 lr = Laplacian(px + offs[0].zw);
    r = saturate(sr.r - k[0].x * lr.r);

    float3 sg = Interpolate(px, wx[1], wy[1], offs[1].xy);
    float3 lg = Laplacian(px + offs[1].zw);
    g = saturate(sg.g - k[1].x * lg.g);

    [branch] if (mode.x != 0) {
        b = saturate(sr.b - k[0].x * lr.b);
    } else {
        float3 sb = Interpolate(px, wx[2], wy[2], offs[2].xy);
        float3 lb = Laplacian(px + offs[2].zw);
        b = saturate(sb.b - k[2].x * lb.b);
    }
    return float4(r, g, b, 1);
}
