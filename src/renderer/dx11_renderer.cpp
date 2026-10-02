#include "../common/input_limits.h"
#include "dx11_renderer.h"
#include "renderer/hdr_tone_map.h"
#include "renderer/hdr_tone_map_hlsl.h"
#include <d3dcompiler.h>
#include <array>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <debugapi.h>
#include <wincodec.h>
#include <DirectXPackedVector.h>  // XMConvertFloatToHalf: scRGB FP16 HDR screenshots
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace NitLink {

static std::wstring HrToHex(HRESULT hr);

// Vertex shader: passes UV through, applies aspect-ratio-correct transform.
// The scale comes from a constant buffer to letterbox/pillarbox correctly
// regardless of how the user resizes the window.
static const char* g_vertexShaderSrc = R"(
cbuffer Transform : register(b0) {
    float2 scale;
    float2 padding;
};

struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.pos = float4(input.pos * scale, 0.0f, 1.0f);
    output.uv  = input.uv;
    return output;
}
)";

// BGRA pixel shader with Catmull-Rom bicubic filtering.
// This is the same algorithm video players like mpv and madVR use -- sharp
// without the ringing artifacts of plain bicubic, and dramatically crisper
// than bilinear when downscaling from 4K to a smaller window.
static const char* g_pixelShaderBGRA = R"(
Texture2D    captureTexture : register(t0);
SamplerState captureSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Catmull-Rom bicubic sampling: based on the reference implementation by
// Matt Pettineo (TheRealMJP) at github.com/TheRealMJP, MIT licensed.
//
//   https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
//
// Why this code instead of a homegrown version (a prior attempt produced
// bluish ringing):
//
//   The kernel has negative outer-tap weights, which is what makes bicubic
//   look sharper than bilinear. On high-contrast edges, those negative weights
//   can drive RGB channels below zero: clamped to 0 by UNORM, which can
//   produce a noticeable color cast on dark pixels next to bright edges
//   (R=0, G=0, B=38 was observed on what should have been near-black).
//
//   MJP's implementation gets several subtle details right that the
//   first-pass NitLink code got wrong:
//     * texPos1 = floor(samplePos - 0.5) + 0.5  (correct half-pixel offset
//       for DX10+ texel-center semantics; the prior code had `floor(samplePos)` only)
//     * SampleLevel(..., 0.0f) instead of Sample(...) (locks LOD to 0,
//       avoiding driver-side mip selection variation)
//     * Pre-expanded weight polynomials in Horner form
//       (numerically stable + fewer ALU ops, per pixelmager + aras-p
//       optimization in the gist comments)
//
// The 9-tap variant is used. The 5-tap variant (which omits corner samples)
// would be ~10% faster but slightly less accurate at high contrast: fine
// for TAA, not what is wanted for a pristine viewer.
//
// Reference benchmarks from MJP on GTX 980 at 1080p: ~0.32ms (1 tap) vs
// ~0.32ms (9 taps). Negligible cost difference; quality is the win.
//
// The negative-weight clamp added below the call is still needed: even
// with the correct implementation, math can dip below 0 on extreme edges,
// and no negative output should reach the swap chain.

float4 sampleCatmullRom(Texture2D tex, SamplerState samp, float2 uv) {
    uint w, h;
    tex.GetDimensions(w, h);
    float2 texSize  = float2(w, h);

    // Sample a 4x4 grid of texels surrounding the target UV.
    // Round down to the exact center of the "starting" texel at grid pos [1,1].
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;

    // Fractional offset from starting texel to original sample location
    float2 f = samplePos - texPos1;

    // Catmull-Rom weights pre-expanded in Horner form (faster + more stable)
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    // Combine middle two weights into one bilinear-sampled tap (the trick that
    // turns 16 texture loads into 9 with no quality loss)
    float2 w12      = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    // Final UV coordinates for the 9 samples
    float2 texPos0  = texPos1 - 1;
    float2 texPos3  = texPos1 + 2;
    float2 texPos12 = texPos1 + offset12;

    texPos0  /= texSize;
    texPos3  /= texSize;
    texPos12 /= texSize;

    float4 result = 0.0f;
    result += tex.SampleLevel(samp, float2(texPos0.x,  texPos0.y),  0.0f) * w0.x  * w0.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos0.y),  0.0f) * w12.x * w0.y;
    result += tex.SampleLevel(samp, float2(texPos3.x,  texPos0.y),  0.0f) * w3.x  * w0.y;

    result += tex.SampleLevel(samp, float2(texPos0.x,  texPos12.y), 0.0f) * w0.x  * w12.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos12.y), 0.0f) * w12.x * w12.y;
    result += tex.SampleLevel(samp, float2(texPos3.x,  texPos12.y), 0.0f) * w3.x  * w12.y;

    result += tex.SampleLevel(samp, float2(texPos0.x,  texPos3.y),  0.0f) * w0.x  * w3.y;
    result += tex.SampleLevel(samp, float2(texPos12.x, texPos3.y),  0.0f) * w12.x * w3.y;
    result += tex.SampleLevel(samp, float2(texPos3.x,  texPos3.y),  0.0f) * w3.x  * w3.y;

    return result;
}

// Pixel-shader constant buffer: runtime knobs for the color pipeline.
// Single CB at b0 (VS's b0 is also used, but slots are per-stage so no clash).
// Layout is one float4 (16 bytes, D3D11 minimum), so slots 2 and 3 are
// preserved for future use even though the current pipeline doesn't read
// them. The CPU-side cbuffer upload writes all four floats every frame.
cbuffer Pixel : register(b0) {
    float colorExpansion;       // 0 = passthrough, 1 = full limited->full expansion
    float hdrMode;              // 0 = SDR output, 1 = HDR scRGB output
    float _reservedSlot2;       // Reserved (used by P010 path for BT.709 fallback)
    float sourceTopDown;        // 0 = bottom-up source (default, VB handles flip),
                                 // 1 = top-down source (BGRA shader must flip V back)
    float sourceFullRange;      // 0 = limited 16-235 (default, do limited-to-full expand),
                                 // 1 = full 0-255 (driver already did it; skip expand)
    float _reservedSlot5;
    float _reservedSlot6;
    float _reservedSlot7;
};

// SMPTE ST.2084 (PQ) inverse EOTF.
// Input:  PQ-encoded float in [0..1]  (as the capture card delivers)
// Output: linear light in NITS [0..10000]
// Constants from ITU-R BT.2100. The form below is the exact non-linear inverse
// of the PQ EOTF; clamping the input keeps NaNs out if the source pixels are
// already slightly out of range (some drivers send 1.001 for highlights).
float3 PQ_to_Nits(float3 pq)
{
    const float m1 = 0.1593017578125;        //  1305/8192
    const float m2 = 78.84375;                //  2523/32
    const float c1 = 0.8359375;               //  107/128
    const float c2 = 18.8515625;              //  2413/128
    const float c3 = 18.6875;                 //  2392/128

    pq = saturate(pq);
    float3 p = pow(pq, 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

// SMPTE ST.2084 (PQ) forward EOTF (Nits → PQ-encoded).
// Input:  linear-light nits [0..10000]
// Output: PQ-encoded float [0..1] suitable for an R10G10B10A2 HDR10 swap chain
//
// This is needed when content is linear (e.g. SDR sRGB content scaled to
// a paper-white nit value) and must be encoded into the PQ curve to
// write into the HDR10 swap chain. The P010 capture path doesn't need this:
// its values are already PQ-encoded.
float3 Nits_to_PQ(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 L = max(nits / 10000.0, 0.0);    // normalize to [0..1]
    float3 Lm1 = pow(L, m1);
    float3 num = c1 + c2 * Lm1;
    float3 den = 1.0 + c3 * Lm1;
    return pow(num / den, m2);
}

// HDR-specific Catmull-Rom resample.
//
// CRITICAL: PQ is a heavily non-linear curve. Blending PQ-encoded codes and
// then decoding produces a different result than decoding each tap and then
// blending, because the PQ curve compresses 10000 nits of dynamic range into
// 0..1 with very different rates of change at different brightness levels.
//
// Concretely: a bright pixel (PQ=0.8 = ~600 nits) and a darker pixel
// (PQ=0.5 = ~80 nits) blended at PQ-space midpoint (PQ=0.65) decodes to
// ~150 nits, NOT the linear-light average of (600+80)/2 = 340 nits. The R
// and G channels traverse the PQ curve at different slopes for orange hues
// (high R, mid G), so the channel ratio drifts during the blend → orange
// hue tears into yellow + red.
//
// Fix: decode each tap to linear light BEFORE applying the Catmull-Rom
// weights. The blend now happens in physically-meaningful units (nits).
//
// Trade-off: 9 extra PQ_to_Nits() calls per pixel vs 1. PQ has a couple of
// pow() ops which are not free, but at 4K this is still well under the
// frame budget on any HDR-capable GPU (RTX 5080 included).
float3 sampleCatmullRomHDR(Texture2D tex, SamplerState samp, float2 uv) {
    uint w, h;
    tex.GetDimensions(w, h);
    float2 texSize  = float2(w, h);

    // Same MJP-style sampling pattern as the BGRA path, but the taps are
    // PQ-decoded to linear nits BEFORE weighting (see the rationale above).
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    float2 f         = samplePos - texPos1;

    // Horner-form Catmull-Rom weights (same as the BGRA path)
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12      = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    float2 texPos0  = (texPos1 - 1.0f) / texSize;
    float2 texPos3  = (texPos1 + 2.0f) / texSize;
    float2 texPos12 = (texPos1 + offset12) / texSize;

    // Fetch raw PQ taps and decode each to linear-light nits BEFORE weighting.
    float3 s0 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos0.x,  texPos0.y),  0.0f).rgb);
    float3 s1 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos12.x, texPos0.y),  0.0f).rgb);
    float3 s2 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos3.x,  texPos0.y),  0.0f).rgb);
    float3 s3 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos0.x,  texPos12.y), 0.0f).rgb);
    float3 s4 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos12.x, texPos12.y), 0.0f).rgb);
    float3 s5 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos3.x,  texPos12.y), 0.0f).rgb);
    float3 s6 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos0.x,  texPos3.y),  0.0f).rgb);
    float3 s7 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos12.x, texPos3.y),  0.0f).rgb);
    float3 s8 = PQ_to_Nits(tex.SampleLevel(samp, float2(texPos3.x,  texPos3.y),  0.0f).rgb);

    float3 result = 0.0;
    result += s0 * w0.x  * w0.y;
    result += s1 * w12.x * w0.y;
    result += s2 * w3.x  * w0.y;
    result += s3 * w0.x  * w12.y;
    result += s4 * w12.x * w12.y;
    result += s5 * w3.x  * w12.y;
    result += s6 * w0.x  * w3.y;
    result += s7 * w12.x * w3.y;
    result += s8 * w3.x  * w3.y;
    return result;
}

// BT.2020 -> BT.709 primaries matrix. NOT USED in the current pipeline:
// colors stay in BT.2020 and are presented via
// DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P2020.
// Kept here in case a future fallback path lands on BT.709 output.
//
// Source: ITU-R BT.2087, derivable from BT709_to_XYZ * inv(BT2020_to_XYZ).
static const float3x3 BT2020_to_BT709 = float3x3(
     1.6605, -0.5876, -0.0728,
    -0.1246,  1.1329, -0.0083,
    -0.0182, -0.1006,  1.1187
);

float4 main(PS_INPUT input) : SV_TARGET {
    // Compute orientation-aware UV. The shared vertex buffer pre-flips V to
    // handle Media Foundation's traditional bottom-up RGB rows. If the
    // active capture source is actually top-down (e.g. Elgato 4K S USB),
    // V must be flipped *back* here. sourceTopDown is set by
    // SetSourceRowOrder() from CaptureFormat::topDown, which itself comes
    // from querying MF_MT_DEFAULT_STRIDE at format negotiation time.
    float2 uv = (sourceTopDown > 0.5)
                  ? float2(input.uv.x, 1.0 - input.uv.y)
                  : input.uv;

    // ============ SDR-IN-HDR PATH ============
    // When the renderer is in HDR10 output mode but the capture is BGRA
    // SDR, sRGB BT.709 values can't just be written into the R10G10B10A2
    // PQ BT.2020 swap chain: they'd be reinterpreted as PQ-encoded BT.2020
    // and land deep in the BT.2020 green primary, producing a dark-green
    // cast.
    //
    // Pipeline:
    //   BGRA sRGB sample
    //   -> sRGB inverse EOTF -> linear BT.709 RGB
    //   -> BT.709 to BT.2020 primaries matrix (ITU-R BT.2087-0)
    //   -> scale to 203 nits (Windows SDR-in-HDR paper-white reference)
    //   -> PQ-encode -> [0..1] for the R10G10B10A2 HDR10 swap chain.
    //
    // Mirrors the NV12 shader's SDR-IN-HDR path so a diff stays clean if
    // either ever changes.
    if (hdrMode > 0.5) {
        float4 c = sampleCatmullRom(captureTexture, captureSampler, uv);
        c.rgb = saturate(c.rgb);  // anti-ringing clamp (same reason as SDR path)

        // sRGB inverse EOTF (proper piecewise: not the gamma 2.2 hack)
        float3 srgb = c.rgb;
        float3 lin709;
        lin709.r = (srgb.r <= 0.04045) ? srgb.r / 12.92
                                       : pow((srgb.r + 0.055) / 1.055, 2.4);
        lin709.g = (srgb.g <= 0.04045) ? srgb.g / 12.92
                                       : pow((srgb.g + 0.055) / 1.055, 2.4);
        lin709.b = (srgb.b <= 0.04045) ? srgb.b / 12.92
                                       : pow((srgb.b + 0.055) / 1.055, 2.4);

        // BT.709 to BT.2020 primaries (ITU-R BT.2087-0). The backbuffer
        // is interpreted by DWM as BT.2020 PQ; without this step, dark
        // BT.709 values get yanked toward the (more saturated) BT.2020
        // green primary and the picture takes on a heavy green cast.
        // Row sums about 1 so neutrals (R=G=B) pass through unchanged.
        float3 lin2020;
        lin2020.r = 0.6274 * lin709.r + 0.3293 * lin709.g + 0.0433 * lin709.b;
        lin2020.g = 0.0691 * lin709.r + 0.9195 * lin709.g + 0.0114 * lin709.b;
        lin2020.b = 0.0164 * lin709.r + 0.0880 * lin709.g + 0.8956 * lin709.b;

        // SDR diffuse white maps to 203 nits (Windows default SDR-in-HDR
        // reference). lin in [0..1] becomes nits in [0..203].
        float3 nits = lin2020 * 203.0;

        // PQ-encode for the HDR10 swap chain.
        return float4(Nits_to_PQ(nits), 1.0);
    }

    // ============ SDR PATH ============
    float4 c = sampleCatmullRom(captureTexture, captureSampler, uv);
    // Clamp Catmull-Rom output to [0,1]. The bicubic kernel has NEGATIVE
    // outer-tap weights (that's how it achieves sharpness) but on high
    // contrast edges (white text on black) the negative weights can push
    // adjacent dark pixels into negative values per channel. When a UNORM
    // backbuffer receives negative output it clamps to 0 anyway, but with
    // ASYMMETRIC effects: blue channel often survives a bit because the
    // text/UI isn't pure white (often a touch of blue), so it lands in
    // positive-weight territory while R and G get crushed. Result: dark
    // areas next to bright edges develop a blue/cyan tint. Saturating here
    // costs a tiny amount of edge sharpness in exchange for clean blacks
    // everywhere. The visual cost is essentially invisible; the bug fix
    // is the difference between #000026 and #0b0a12 on dark backgrounds.
    c.rgb = saturate(c.rgb);

    // PS5 (and almost all consoles over HDMI) outputs LIMITED-range RGB:
    //   black = 16/255, white = 235/255, instead of black = 0, white = 255.
    // Media Foundation's RGB32 conversion preserves this range, so without
    // expansion blacks look grey and whites look dim.
    //
    // Expand: out = (in - 16/255) * 255/219
    //   - subtracts the black offset
    //   - scales the remaining 219 levels back to the full 0..1 range
    // Clamp at the end to prevent any out-of-range super-blacks/whites from
    // poorly-mastered sources.
    //
    // colorExpansion is lerped 0..1 so toggling it in the settings panel is
    // smooth instead of a hard pop.
    //
    // If the driver reports the source is already full-range (e.g. some
    // NV12 paths), skip the expansion entirely: applying it would crush
    // blacks (values below 16/255 -> negative -> clamped to 0).
    const float blackPoint = 16.0 / 255.0;
    const float scale      = 255.0 / 219.0;
    float3 expanded = saturate((c.rgb - blackPoint) * scale);
    float effectiveExpansion = (sourceFullRange > 0.5) ? 0.0 : colorExpansion;
    c.rgb = lerp(c.rgb, expanded, effectiveExpansion);

    return c;
}
)";

// NV12 -> RGB pixel shader (Media Foundation default format for 4K capture)
static const char* g_pixelShaderNV12 = R"(
Texture2D    yPlane  : register(t0);
Texture2D    uvPlane : register(t1);
SamplerState captureSampler : register(s0);

// Shared CB layout: must match g_pixelShaderBGRA so the renderer can bind
// the same constant buffer (m_pixelCB) for all capture-format shaders.
cbuffer Pixel : register(b0) {
    float colorExpansion;
    float hdrMode;
    float _reservedSlot2;
    float sourceTopDown;        // 0 = bottom-up source, 1 = top-down source
    float sourceFullRange;      // 0 = limited 16-235, 1 = full 0-255
    float _reservedSlot5;
    float _reservedSlot6;
    float _reservedSlot7;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// BT.709 YUV -> RGB. Handles both limited-range and full-range YUV.
//
// Limited range (the HDMI default): Y in [16/255 .. 235/255], UV centered
// at 128/255 with valid span [16/255 .. 240/255]. Subtract the offsets
// and scale Y by 255/219, UV by 255/224 so a "full white" input maps to
// 1.0 RGB output.
//
// Full range: Y in [0..1], UV centered at 128/255 with span [0..1]. No
// scaling, just bias removal on UV. Most HDMI sources output limited
// range, but some capture-card drivers convert to full-range YUV before
// handing it to the host (the Elgato 4K S does this on its NV12 path).
// Without the branch, applying the limited-range expansion to full-range
// Y crushes blacks (Y=0 -> Y=-16/255 -> clamped to 0, losing the lowest
// 6% of the dynamic range).
float3 yuvToRgb(float y, float u, float v, bool fullRange) {
    if (fullRange) {
        u = u - 128.0/255.0;
        v = v - 128.0/255.0;
    } else {
        y = (y - 16.0/255.0)  * (255.0/219.0);
        u = (u - 128.0/255.0) * (255.0/224.0);
        v = (v - 128.0/255.0) * (255.0/224.0);
    }

    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    return saturate(float3(r, g, b));
}

// SMPTE ST.2084 (PQ) forward EOTF: used by the SDR-in-HDR branch below.
// Same function as in the BGRA shader; duplicated here because each shader
// is compiled from an independent source string and HLSL has no shared
// includes in this build path. If a third HDR branch lands, factor this
// into a shared header. Comment-and-constants kept identical to BGRA's copy
// so a diff stays clean if either ever changes.
float3 Nits_to_PQ(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 L = max(nits / 10000.0, 0.0);
    float3 Lm1 = pow(L, m1);
    float3 num = c1 + c2 * Lm1;
    float3 den = 1.0 + c3 * Lm1;
    return pow(num / den, m2);
}

float4 main(PS_INPUT input) : SV_TARGET {
    // Orientation-aware UV: see BGRA shader main() for full explanation.
    float2 uv = (sourceTopDown > 0.5)
                  ? float2(input.uv.x, 1.0 - input.uv.y)
                  : input.uv;

    float  y     = yPlane.Sample (captureSampler, uv).r;
    float2 uvSmp = uvPlane.Sample(captureSampler, uv).rg;
    bool   full  = (sourceFullRange > 0.5);
    float3 rgb   = yuvToRgb(y, uvSmp.x, uvSmp.y, full);

    // ============ SDR-IN-HDR PATH ============
    // When the renderer is in HDR10 output mode but the capture is an SDR
    // NV12 stream (PS5 in SDR menu, HDR game not launched yet, etc), do
    // a proper SDR-to-PQ conversion so the picture appears at the right
    // brightness and color with no green cast.
    //
    // Pipeline:
    //   yuvToRgb output (already sRGB-encoded [0..1])
    //   -> sRGB inverse EOTF -> linear BT.709 RGB
    //   -> BT.709 to BT.2020 primaries matrix (ITU-R BT.2087-0)
    //   -> scale to 203 nits (Windows SDR-in-HDR paper-white reference)
    //   -> PQ-encode -> [0..1] for the R10G10B10A2 HDR10 swap chain.
    //
    // This branch only renders correctly when Windows HDR is engaged
    // system-wide (DXGI_OUTPUT_DESC1.ColorSpace == HDR10). With Windows
    // HDR off, DWM compositor doesn't actually take the PQ BT.2020
    // backbuffer to the panel as-is: it routes it through an SDR fallback
    // that mangles channels in unpredictable ways. The C++ side gates
    // SetHDREnabled() on m_hdrEngagedByOS to prevent that scenario.
    if (hdrMode > 0.5) {
        // SDR-in-HDR pipeline. yuvToRgb returns sRGB-encoded BT.709 RGB
        // (the same value the SDR path would write to an sRGB backbuffer).
        // Inverse-EOTF to linear, rotate primaries to BT.2020,
        // scale to 203 nits (Windows SDR-in-HDR paper-white reference),
        // and PQ-encode for the R10G10B10A2 HDR10 swap chain.
        float3 srgb = rgb;

        float3 lin709;
        lin709.r = (srgb.r <= 0.04045) ? srgb.r / 12.92
                                       : pow((srgb.r + 0.055) / 1.055, 2.4);
        lin709.g = (srgb.g <= 0.04045) ? srgb.g / 12.92
                                       : pow((srgb.g + 0.055) / 1.055, 2.4);
        lin709.b = (srgb.b <= 0.04045) ? srgb.b / 12.92
                                       : pow((srgb.b + 0.055) / 1.055, 2.4);

        // BT.709 to BT.2020 primaries (ITU-R BT.2087-0). Row sums about 1
        // so neutrals pass through unchanged; colored content gets rotated
        // so the BT.2020 backbuffer interprets it as the intended BT.709 hue.
        float3 lin2020;
        lin2020.r = 0.6274 * lin709.r + 0.3293 * lin709.g + 0.0433 * lin709.b;
        lin2020.g = 0.0691 * lin709.r + 0.9195 * lin709.g + 0.0114 * lin709.b;
        lin2020.b = 0.0164 * lin709.r + 0.0880 * lin709.g + 0.8956 * lin709.b;

        float3 nits = lin2020 * 203.0;
        return float4(Nits_to_PQ(nits), 1.0);
    }

    // ============ SDR PATH ============
    return float4(rgb, 1.0);
}
)";

// P010 -> R10G10B10A2 HDR10 pixel shader.
//
// P010 is YUV 4:2:0 at 10 bits-per-channel, packed in the top 10 bits of
// 16-bit values. Y plane sampled as R16_UNORM, UV plane as R16G16_UNORM
// (half-res: the GPU sampler handles upsampling).
//
// The PS5 over HDMI in HDR10 mode sends FULL-RANGE BT.2020 PQ. The Elgato
// presents this as P010 with the raw PQ-encoded values intact (the card's
// internal hardware tonemap is disabled at startup). The swap chain is also
// HDR10 (R10G10B10A2_UNORM + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
// so the values flow through end-to-end in their native encoding:
//
//   PS5 -> P010 (BT.2020 PQ) -> YUV-to-RGB -> R10G10B10A2 (BT.2020 PQ) -> display
//
// NO PQ decode, NO BT.2020-to-BT.709 conversion, NO gamut compression.
// Just the YUV-to-RGB step that's intrinsic to YUV's encoding. Output
// goes directly to the HDR10 swap chain, which DWM passes through to
// the display without further conversion.
//
// This is the "native HDR10 video player" approach: the same path
// Windows uses for HDR10 video playback in MPV/madVR/etc.
static const char* g_pixelShaderP010 = R"(
Texture2D    yPlane  : register(t0);
Texture2D    uvPlane : register(t1);
SamplerState captureSampler : register(s0);

// Cbuffer layout mirrors the NV12/BGRA shaders so the C++ upload path can
// be shared. P010 doesn't currently consume slots 0..3 (colorExpansion is
// SDR-only, hdrMode is implicit because the P010 shader only runs in HDR
// mode, sourceTopDown is hardcoded below since MF always delivers P010
// top-down), but the slot numbers stay reserved to keep the C++ upload
// agnostic of which shader is bound. Slot 4 IS consumed: the PS5 emits
// limited-range YUV by default (PS5 RGB Range setting; matches the MF
// Nominal range="not set, assuming limited" log line), so the same
// limited-to-full expansion the SDR path does is required: without it, Y arrives
// compressed in [64/1023..940/1023], hue rotates toward the BT.2020 green
// primary, blacks lift, and the picture goes green-cast (exact symptom
// surfaced once Alt+H reconcile started feeding real P010 frames through
// the HDR pipeline).
// Slot 6 is consumed only for the MK.2 limited-P010 chroma policy.
cbuffer PixelCB : register(b0) {
    float colorExpansion;
    float hdrMode;
    float _reservedSlot2;
    float _reservedSlot3;
    // 0 = limited-range source (10-bit Y in 64/1023..940/1023, MK.2 chroma in
    //     64/1023..960/1023). Run yuv2020ToRgb_Limited which expands those
    //     ranges before the BT.2020 matrix; other cards expand only luma.
    // 1 = full-range source (Y in 0..1, chroma in 0..1). Use
    //     yuv2020ToRgb_Full directly.
    // Driven from Application::UpdateCaptureColorInterpretation, which mirrors
    // CaptureFormat::fullRange (set from MF_MT_VIDEO_NOMINAL_RANGE).
    float sourceFullRange;
    float sdrFromHdrTonemap;  // 1.0 = P010 source but SDR backbuffer; do
                              // PQ -> linear -> BT.709 -> luminance EETF -> sRGB
                              // in shader. 0.0 = normal HDR path (PQ
                              // BT.2020 passthrough) or any non-HDR source.
    float p010LimitedChroma;  // MK.2: standard 64..960 chroma; otherwise legacy
    float _reservedSlot7;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Full-range BT.2020 YUV -> RGB. Coefficients from ITU-R BT.2020:
// Kr = 0.2627, Kb = 0.0593, Kg = 0.6780. For FULL range:
//   R = Y + 1.4746 * (V - 0.5)
//   G = Y - 0.16455 * (U - 0.5) - 0.57135 * (V - 0.5)
//   B = Y + 1.8814 * (U - 0.5)
// Note: the output is still PQ-ENCODED. YUV-to-RGB is a linear matrix
// operation; PQ decoding doesn't happen here because it isn't needed:
// the HDR10 swap chain expects PQ-encoded values in [0..1] directly.
float3 yuv2020ToRgb_Full(float y, float u, float v) {
    float U = u - 0.5;
    float V = v - 0.5;
    float r = y + 1.4746    * V;
    float g = y - 0.16455   * U - 0.57135 * V;
    float b = y + 1.8814    * U;
    return float3(r, g, b);
}

// Limited-range LUMA, full-range CHROMA BT.2020 10-bit YUV -> RGB by default. P010 samples
// come from R16_UNORM / R16G16_UNORM with the 10-bit data in the high bits,
// mapped to [0..1] by the sampler. The HDR10 sources seen here on the 4K Pro carry limited-
// range luma (valid Y in [64/1023..940/1023], range 876) but full-range chroma.
// Confirmed on the Elgato 4K Pro against the PS5 shown native on the TV
// passthrough: full chroma matches the color exactly, while expanding the chroma
// as if limited over-saturates it (orange skin tones). Leaving the luma
// unexpanded separately lifts blacks to a milky grey, so expand only the luma.
// MK.2 is the exception: limited P010 chroma uses codes 64..960, centered at 512.
// Decode its high-bit-aligned R16_UNORM samples with 65535/64 normalization.
float3 yuv2020ToRgb_Limited(float y, float u, float v) {
    float y_full = (y * 1023.0 - 64.0) / 876.0;
    if (p010LimitedChroma > 0.5) {
        u = 0.5 + (u * (65535.0 / 64.0) - 512.0) / 896.0;
        v = 0.5 + (v * (65535.0 / 64.0) - 512.0) / 896.0;
    }
    return yuv2020ToRgb_Full(y_full, u, v);
}

// ============== HDR-to-SDR tonemap helpers (P010 SDR-output path) ==============
//
// When the source is HDR10 but the user wants SDR output (Alt+H off while
// a PS5 HDR game is running), tonemap in-shader rather than relying on
// the Elgato hardware tonemap (which doesn't reliably re-engage mid-session
// on this firmware). Pipeline:
//   1. yuv2020ToRgb_Limited       -> PQ-encoded BT.2020 RGB
//   2. PqToLinear                 -> linear BT.2020 in [0..10000] nits
//   3. Bt2020ToBt709_Linear       -> linear BT.709
//   4. BT.2446A-derived EETF      -> SDR-range linear [0..1]
//   5. LinearToSrgb               -> gamma-encoded sRGB for BGRA8 output

// SMPTE ST.2084 (PQ) inverse EOTF. Input: PQ-encoded value in [0..1].
// Output: linear luminance normalized so 1.0 == 10000 nits, matching the
// HDR10 spec maximum.
float3 PqToLinear(float3 pq) {
    const float m1 = 0.1593017578125;       // 2610 / 16384
    const float m2 = 78.84375;              // 2523 / 4096 * 128
    const float c1 = 0.8359375;             // 3424 / 4096
    const float c2 = 18.8515625;            // 2413 / 4096 * 32
    const float c3 = 18.6875;               // 2392 / 4096 * 32
    float3 p  = pow(max(pq, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return pow(num / den, 1.0 / m1);
}

// BT.2020 to BT.709 primaries conversion in linear light. Out-of-gamut
// channels remain intact here and are compressed only after tone mapping.
float3 Bt2020ToBt709_Linear(float3 c) {
    return float3(
         1.6605 * c.r - 0.5876 * c.g - 0.0728 * c.b,
        -0.1246 * c.r + 1.1329 * c.g - 0.0083 * c.b,
        -0.0182 * c.r - 0.1006 * c.g + 1.1187 * c.b
    );
}

// NITLINK_BT2446_DERIVED_LUMINANCE_FUNCTION

float3 CompressToSdrGamut(float3 c, float mappedY) {
    float chromaScale = 1.0;
    float3 delta = c - mappedY.xxx;

    if (delta.r < 0.0) chromaScale = min(chromaScale, -mappedY / delta.r);
    else if (delta.r > 0.0) chromaScale = min(chromaScale, (1.0 - mappedY) / delta.r);
    if (delta.g < 0.0) chromaScale = min(chromaScale, -mappedY / delta.g);
    else if (delta.g > 0.0) chromaScale = min(chromaScale, (1.0 - mappedY) / delta.g);
    if (delta.b < 0.0) chromaScale = min(chromaScale, -mappedY / delta.b);
    else if (delta.b > 0.0) chromaScale = min(chromaScale, (1.0 - mappedY) / delta.b);

    return saturate(mappedY.xxx + delta * saturate(chromaScale));
}

float3 ToneMapBt709Luminance(float3 linear709) {
    const float3 bt709Luma = float3(0.2126, 0.7152, 0.0722);
    const float originalY = dot(linear709, bt709Luma);
    if (originalY <= 1e-6) return 0.0;

    const float mappedY = ToneMapLuminanceNits(originalY * 10000.0);
    return CompressToSdrGamut(linear709 * (mappedY / originalY), mappedY);
}

// Linear to sRGB EOTF (gamma encode). Standard piecewise function from
// IEC 61966-2-1. Writes the result straight into the BGRA8_UNORM swap
// chain backbuffer (which is interpreted as sRGB by DWM).
float3 LinearToSrgb(float3 lin) {
    lin = saturate(lin);
    float3 lo = lin * 12.92;
    float3 hi = 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
    return (lin <= 0.0031308) ? lo : hi;
}

float4 main(PS_INPUT input) : SV_TARGET {
    // The shared fullscreen-quad VS already pre-flips V to compensate for
    // Media Foundation's bottom-up RGB rows. P010 from MF is *top-down*
    // (planar formats use natural orientation), so V must be flipped back.
    // Without this the HDR image renders upside-down.
    float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

    // P010 stores 10-bit values in the top 10 bits of 16-bit channels;
    // R16_UNORM / R16G16_UNORM sampling returns the full 16-bit value
    // mapped to [0..1]. Bottom 6 bits being zero just means 10-bit
    // precision lands in a 16-bit container: perfect.
    float  y  = yPlane.Sample (captureSampler, uv).r;
    float2 uvSample = uvPlane.Sample(captureSampler, uv).rg;

    // ---- SDR-output branch (user toggled HDR off while source is HDR10) ----
    // Reached on the 4K Pro path when m_sourceIsHDR10 is true and the user
    // disables HDR rendering via Alt+H. The capture stays P010 (the source
    // is still HDR10) but the renderer's backbuffer is SDR BGRA8, so the
    // shader tonemaps in-shader. On the 4K S this branch effectively never
    // executes: the HID tonemap-state machine flips the card into hardware
    // SDR mode and the capture format is renegotiated to NV12, so an
    // HDR-off + P010-source combination never reaches this shader.
    //   Pipeline: BT.2020 PQ YUV -> RGB -> PQ inverse EOTF -> linear BT.2020
    //   -> linear BT.709 -> BT.2446A-derived luminance EETF
    //   -> sRGB EOTF -> 8-bit BGRA backbuffer.
    if (sdrFromHdrTonemap > 0.5) {
        float3 rgb;
        if (sourceFullRange > 0.5) {
            rgb = yuv2020ToRgb_Full(y, uvSample.x, uvSample.y);
        } else {
            rgb = yuv2020ToRgb_Limited(y, uvSample.x, uvSample.y);
        }
        float3 linear2020 = PqToLinear(rgb);
        float3 linear709  = Bt2020ToBt709_Linear(linear2020);
        float3 sdrLinear  = ToneMapBt709Luminance(linear709);
        float3 srgb       = LinearToSrgb(sdrLinear);
        return float4(srgb, 1.0);
    }

    // ---- HDR-output: BT.2020 PQ passthrough ----
    // PS5 sends limited-range Y'CbCr by default; the limited helper expands
    // [64/1023..940/1023] -> [0..1] for Y; MK.2 also expands limited chroma
    // from 64..960 before applying the BT.2020 matrix. saturate() handles
    // out-of-spec overshoots from the expansion at the very brightest
    // pixels. Output is still PQ-encoded BT.2020, exactly what the
    // R10G10B10A2 + G2084 swap chain wants.
    float3 rgb;
    if (sourceFullRange > 0.5) {
        rgb = yuv2020ToRgb_Full(y, uvSample.x, uvSample.y);
    } else {
        rgb = yuv2020ToRgb_Limited(y, uvSample.x, uvSample.y);
    }
    return float4(saturate(rgb), 1.0);
}
)";

static bool BuildP010ShaderSource(std::string& source)
{
    source = g_pixelShaderP010;
    return HdrToneMap::InjectDerivedLuminanceFunction(
        source, "// NITLINK_BT2446_DERIVED_LUMINANCE_FUNCTION");
}

// UI overlay shader for HDR compositing.
// Samples BGRA8 UI texture (sRGB, premultiplied alpha), converts to linear
// nits at 203-nit paper-white, then PQ-encodes for the HDR10 swap chain
// (R10G10B10A2 + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020). This matches
// the Windows SDR-in-HDR reference brightness so the menu reads as normal
// SDR content sitting on top of the HDR game image.
static const char* g_pixelShaderUI = R"(
Texture2D    uiTexture : register(t0);
SamplerState uiSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 srgbToLinear(float3 c) {
    return pow(abs(c), 2.2);
}

// Forward PQ EOTF: same constants as the capture shaders; a copy is inlined
// here so the UI shader is self-contained (HLSL doesn't share funcs
// across compile units the way C does, and the shaders here are compiled
// as separate strings).
float3 Nits_to_PQ_UI(float3 nits) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 L = max(nits / 10000.0, 0.0);
    float3 Lm1 = pow(L, m1);
    float3 num = c1 + c2 * Lm1;
    float3 den = 1.0 + c3 * Lm1;
    return pow(num / den, m2);
}

float4 main(PS_INPUT input) : SV_TARGET {
    // The shared fullscreen-quad VS pre-flips V coords because the
    // capture path uses Media Foundation which delivers bottom-up rows.
    // The D2D-rendered UI texture is already top-down, so V is flipped back
    // here to compensate. Without this the overlay renders upside-down.
    float2 uv = float2(input.uv.x, 1.0 - input.uv.y);
    float4 ui = uiTexture.Sample(uiSampler, uv);
    // UI is premultiplied alpha in sRGB. 'linear' is a reserved HLSL
    // interpolation modifier, so the local variable is named 'lin' instead.
    float3 lin = srgbToLinear(ui.rgb);
    // Scale to 203-nit paper-white (Windows default SDR reference) then
    // PQ-encode for the HDR10 swap chain.
    float3 nits = lin * 203.0;
    return float4(Nits_to_PQ_UI(nits), ui.a);
}
)";

struct Vertex {
    float x, y;
    float u, v;
};

// Note: Media Foundation's RGB32/ARGB32 output uses bottom-up row order
// (legacy GDI convention). V coordinates are flipped here so the image
// renders right-side-up without copying/flipping the data CPU-side.
static const Vertex g_quadVertices[] = {
    { -1.0f,  1.0f, 0.0f, 1.0f }, // top-left
    {  1.0f,  1.0f, 1.0f, 1.0f }, // top-right
    { -1.0f, -1.0f, 0.0f, 0.0f }, // bottom-left
    {  1.0f, -1.0f, 1.0f, 0.0f }, // bottom-right
};

struct TransformCB {
    float scaleX;
    float scaleY;
    float pad0;
    float pad1;
};

DX11Renderer::DX11Renderer() = default;
DX11Renderer::~DX11Renderer()
{
    // Unbind every view and drain the context before the members release.
    // A backbuffer view left bound keeps the swap chain alive past this
    // object, and the device-loss rebuild creates its replacement on the
    // same window right after this destructor. Both calls are safe on a
    // removed device.
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    // Close the DXGI 1.3 frame-latency waitable handle if one was created.
    // DXGI owns the swap chain reference, but the handle itself is given
    // out as a Win32 HANDLE that must be closed to avoid a small kernel-
    // handle leak per renderer lifetime.
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
}

bool DX11Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd = hwnd;
    m_windowWidth = width;
    m_windowHeight = height;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = width;
    scd.Height      = height;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // ALLOW_TEARING:               immediate present (no vsync wait)
    // FRAME_LATENCY_WAITABLE_OBJECT: capped queue depth + waitable handle
    //
    // The waitable-object flag is the key to low latency. By default DXGI
    // queues up to 3 frames before they hit the display. Even with immediate
    // present (Present(0)), DXGI just races to fill the queue, leaving the
    // app 1-3 frames behind reality with no way to know. With this flag,
    // a waitable handle (GetFrameLatencyWaitableObject) is available and
    // the queue depth can be capped via SetMaximumFrameLatency(1). Net
    // result: every displayed frame is the freshest one available, never
    // older than ~16ms.
    //
    // This flag CANNOT be added or removed via ResizeBuffers: DXGI returns
    // an error if a later ResizeBuffers passes different flags. So all
    // subsequent ResizeBuffers calls must preserve it too.
    scd.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                    | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    // Debug layer prints GPU-side warnings about state mismatches, hazards,
    // and resource lifecycle issues. Cheap in Debug; disabled in Release
    // because the constant validation tanks performance and floods the
    // Output window. Re-enable temporarily if hunting GPU-state bugs.
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL requestedLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, requestedLevels, 2,
        D3D11_SDK_VERSION,
        &m_device, &featureLevel, &m_context
    );
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] D3D11CreateDevice failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] IDXGIDevice query failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] GetAdapter failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] IDXGIFactory2 query failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    hr = factory->CreateSwapChainForHwnd(
        m_device.Get(), hwnd, &scd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        // DXGI_ERROR_INVALID_CALL here means the window still carries a swap
        // chain, normally one left alive by an object that survived a lost
        // device.
        OutputDebugStringW((L"[NitLink/Renderer] CreateSwapChainForHwnd failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // DXGI 1.3 low-latency setup. The waitable-object flag was requested at
    // creation time; now retrieve the handle and clamp the maximum frame
    // queue depth to 1. This is the canonical Microsoft-recommended pattern
    // for low-input-latency apps.
    //
    // SetMaximumFrameLatency(1):
    //   Default for waitable swap chains is also 1, but set explicitly here
    //   in case a future Windows release changes the default. Increasing
    //   beyond 1 would trade latency for CPU-GPU parallelism: latency wins
    //   here over parallelism, since rendering a fullscreen quad is cheap.
    //
    // GetFrameLatencyWaitableObject:
    //   Returns a handle that DXGI signals when it's ready to accept another
    //   frame. The handle is closed in the destructor.
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(m_swapChain.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        m_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
        if (m_frameLatencyWaitable) {
            OutputDebugStringW(L"[NitLink/Renderer] Low-latency mode: waitable swap chain enabled (max frame latency = 1)\n");
        } else {
            OutputDebugStringW(L"[NitLink/Renderer] WARN: GetFrameLatencyWaitableObject returned null; running without latency wait\n");
        }
    } else {
        OutputDebugStringW(L"[NitLink/Renderer] WARN: IDXGISwapChain2 unavailable; running without latency wait\n");
    }

    // VRR present-rate cap (VRR_CAP.txt next to the exe): rate-cap the ALLOW_TEARING
    // present to just under the display's VRR max, so VRR engages (the panel refreshes
    // on present: no vblank wait, no tearing inside the VRR range). File content =
    // target Hz (default 117). Absent = off. Pairs with the ALLOW_TEARING present.
    if (std::filesystem::exists("VRR_CAP.txt")) {
        m_vrrCapHz = 117.0;
        m_presentCapFromMarker = true;
        std::ifstream capFile("VRR_CAP.txt");
        double hz = 0.0;
        if (capFile >> hz && hz >= 30.0 && hz <= 1000.0) m_vrrCapHz = hz;
        OutputDebugStringW((L"[NitLink/Renderer] VRR cap ON: " + std::to_wstring((int)m_vrrCapHz)
            + L" Hz ALLOW_TEARING present-rate cap\n").c_str());
    }

    if (!CreateRenderTarget()) {
        OutputDebugStringW(L"[NitLink/Renderer] Initialize: CreateRenderTarget failed\n");
        return false;
    }
    if (!CreateFullscreenQuad()) {
        OutputDebugStringW(L"[NitLink/Renderer] Initialize: CreateFullscreenQuad failed\n");
        return false;
    }
    if (!CreateCompositeShader()) {
        OutputDebugStringW(L"[NitLink/Renderer] Initialize: CreateCompositeShader failed\n");
        return false;
    }

    // GPU timestamp queries are a "best effort" telemetry feature. If the
    // driver refuses to create them, the HUD shows "GPU --" but rendering
    // continues normally.
    m_gpuQueryEnabled = CreateGpuTimingQueries();
    if (!m_gpuQueryEnabled) {
        OutputDebugStringW(L"[NitLink/Renderer] GPU timestamp queries unavailable; HUD will show GPU --\n");
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter         = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.MaxAnisotropy  = 16;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&samplerDesc, &m_sampler);
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] CreateSamplerState failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth      = sizeof(TransformCB);
    cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_transformCB);
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] CreateBuffer(TransformCB) failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    // Pixel-shader constant buffer for color pipeline knobs. 16 bytes is the
    // minimum D3D11 will accept (one float4 worth): 4 bytes plus pad.
    D3D11_BUFFER_DESC pcbDesc{};
    pcbDesc.ByteWidth      = 32; // 8 floats: colorExpansion, hdrMode, slot2 (BT.709 fallback), sourceTopDown, fullRange, + 3 reserved
    pcbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    pcbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    pcbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&pcbDesc, nullptr, &m_pixelCB);
    if (FAILED(hr)) {
        OutputDebugStringW((L"[NitLink/Renderer] CreateBuffer(PixelCB) failed " + HrToHex(hr) + L"\n").c_str());
        return false;
    }

    // Probe the connected display for HDR capability. This populates
    // m_hdrDisplaySupported / m_hdrEngagedByOS / luminance bounds so the
    // settings UI can show the right status before the user toggles HDR.
    QueryHDRDisplayInfo();

    return true;
}

bool DX11Renderer::CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    return SUCCEEDED(hr);
}

void DX11Renderer::ReleaseRenderTarget()
{
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
}

bool DX11Renderer::CreatePostInputTarget(uint32_t w, uint32_t h)
{
    ReleasePostInputTarget();
    if (w == 0 || h == 0) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = w;
    desc.Height           = h;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_postInputTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(m_postInputTex.Get(), nullptr, &m_postInputRTV);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_postInputTex.Get(), nullptr, &m_postInputSRV);
    if (FAILED(hr)) return false;

    m_postInputW = w;
    m_postInputH = h;

    std::wstringstream ss;
    ss << L"[NitLink/Renderer] post-input target " << w << L"x" << h << L"\n";
    OutputDebugStringW(ss.str().c_str());
    return true;
}

void DX11Renderer::ReleasePostInputTarget()
{
    m_postInputSRV.Reset();
    m_postInputRTV.Reset();
    m_postInputTex.Reset();
    m_postInputW = 0;
    m_postInputH = 0;
}

// Simple passthrough pixel shader: sample the upscaled texture using the
// vertex shader's existing aspect transform and write to the backbuffer.
// Re-uses the standard vertex shader and aspect-CB pipeline already set up
// for the capture draw.
static const char* g_compositePS = R"(
Texture2D    srcTexture : register(t0);
SamplerState srcSampler : register(s0);
struct PS_INPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PS_INPUT i) : SV_TARGET {
    // The shared fullscreen-quad VS uses V coords PRE-FLIPPED to compensate
    // for Media Foundation's bottom-up native frame layout when sampling
    // m_captureTexture. By the time data reaches this composite pass it's
    // already been flipped once (DrawCaptureFrame wrote it right-side-up
    // into the intermediate). So V is flipped again here to cancel the VS's
    // pre-flip and end up sampling the intermediate the natural way.
    float2 uv = float2(i.uv.x, 1.0 - i.uv.y);
    return srcTexture.Sample(srcSampler, uv);
}
)";

bool DX11Renderer::CreateCompositeShader()
{
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(g_compositePS, strlen(g_compositePS), nullptr, nullptr, nullptr,
                             "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &blob, &err);
    if (FAILED(hr)) {
        OutputDebugStringW(L"[NitLink/Renderer] composite PS compile failed\n");
        return false;
    }
    hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                       nullptr, &m_compositePS);
    if (FAILED(hr)) return false;

    // UI compositor shader: samples a BGRA8 sRGB texture and converts to
    // scRGB so the SDR-authored overlay text/icons appear at correct
    // brightness over an HDR (FP16) backbuffer. Compiled alongside the
    // composite shader so it's always available: the renderer doesn't
    // know at init time whether HDR will be enabled later.
    {
        ComPtr<ID3DBlob> uiBlob, uiErr;
        HRESULT uiHr = D3DCompile(g_pixelShaderUI, strlen(g_pixelShaderUI),
                                    nullptr, nullptr, nullptr,
                                    "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                    &uiBlob, &uiErr);
        if (FAILED(uiHr)) {
            OutputDebugStringW(L"[NitLink/Renderer] UI PS compile failed:\n");
            if (uiErr) {
                // Dump the compiler's actual error so it shows up in the
                // debug output. Convert ASCII to wide for OutputDebugStringW.
                const char* msg = (const char*)uiErr->GetBufferPointer();
                int wlen = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
                std::wstring wmsg(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg.data(), wlen);
                OutputDebugStringW(wmsg.c_str());
                OutputDebugStringW(L"\n");
            }
            // Non-fatal: HDR overlay will just be missing, SDR still works.
        } else {
            m_device->CreatePixelShader(uiBlob->GetBufferPointer(), uiBlob->GetBufferSize(),
                                          nullptr, &m_pixelShaderUI);
        }
    }

    return true;
}

bool DX11Renderer::CreateFullscreenQuad()
{
    ComPtr<ID3DBlob> vsBlob, psBgraBlob, psNv12Blob, psP010Blob, errorBlob;

    HRESULT hr = D3DCompile(
        g_vertexShaderSrc, strlen(g_vertexShaderSrc),
        "VS", nullptr, nullptr, "main", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &vsBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[NitLink/Renderer] VS compile error:\n");
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
            OutputDebugStringA("\n");
        } else {
            OutputDebugStringA("[NitLink/Renderer] VS compile FAILED (no error blob)\n");
        }
        return false;
    }

    hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &m_vertexShader);
    if (FAILED(hr)) return false;

    hr = D3DCompile(
        g_pixelShaderBGRA, strlen(g_pixelShaderBGRA),
        "PS", nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &psBgraBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[NitLink/Renderer] BGRA PS compile error:\n");
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
            OutputDebugStringA("\n");
        } else {
            OutputDebugStringA("[NitLink/Renderer] BGRA PS compile FAILED (no error blob)\n");
        }
        return false;
    }
    hr = m_device->CreatePixelShader(
        psBgraBlob->GetBufferPointer(), psBgraBlob->GetBufferSize(),
        nullptr, &m_pixelShaderBGRA);
    if (FAILED(hr)) return false;

    hr = D3DCompile(
        g_pixelShaderNV12, strlen(g_pixelShaderNV12),
        "PS", nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &psNv12Blob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[NitLink/Renderer] NV12 PS compile error:\n");
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
            OutputDebugStringA("\n");
        } else {
            OutputDebugStringA("[NitLink/Renderer] NV12 PS compile FAILED (no error blob)\n");
        }
        return false;
    }
    // Log shader source byte count. Useful sanity check after shader edits:
    // if you change the source and the size doesn't move, the old binary is
    // still being loaded somehow.
    {
        char buf[128];
        sprintf_s(buf, "[NitLink/Renderer] NV12 PS compiled OK, source size=%zu bytes\n",
                  strlen(g_pixelShaderNV12));
        OutputDebugStringA(buf);
    }
    hr = m_device->CreatePixelShader(
        psNv12Blob->GetBufferPointer(), psNv12Blob->GetBufferSize(),
        nullptr, &m_pixelShaderNV12);
    if (FAILED(hr)) return false;

    // P010 (HDR10) shader. Compiled even when not currently used so it's
    // ready if the user toggles HDR on while an HDR10 source is present.
    std::string p010ShaderSource;
    if (!BuildP010ShaderSource(p010ShaderSource)) {
        OutputDebugStringA("[NitLink/Renderer] P010 tone-map marker missing\n");
        return false;
    }
    const HdrToneMap::HlslMacroSet toneMapMacros;
    hr = D3DCompile(
        p010ShaderSource.data(), p010ShaderSource.size(),
        "PS", toneMapMacros.macros.data(), nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &psP010Blob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[NitLink/Renderer] P010 PS compile error:\n");
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
            OutputDebugStringA("\n");
        } else {
            OutputDebugStringA("[NitLink/Renderer] P010 PS compile FAILED (no error blob)\n");
        }
        return false;
    }
    hr = m_device->CreatePixelShader(
        psP010Blob->GetBufferPointer(), psP010Blob->GetBufferSize(),
        nullptr, &m_pixelShaderP010);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,                 D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = m_device->CreateInputLayout(
        layout, 2,
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &m_inputLayout);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(g_quadVertices);
    vbDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = g_quadVertices;

    hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    return SUCCEEDED(hr);
}

bool DX11Renderer::CreateCaptureResources(uint32_t width, uint32_t height, bool isNV12)
{
    ForgetResyncFrame(CaptureFrameHoldReason::ResourcesRecreated);
    m_captureTexture.Reset();
    m_captureSRV.Reset();
    m_captureSRV_UV.Reset();

    if (isNV12) {
        // Single NV12 texture, two SRVs (Y plane as R8, UV plane as R8G8)
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width     = width;
        texDesc.Height    = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format    = DXGI_FORMAT_NV12;
        texDesc.SampleDesc = {1, 0};
        texDesc.Usage     = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_captureTexture);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC ySrv{};
        ySrv.Format = DXGI_FORMAT_R8_UNORM;
        ySrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        ySrv.Texture2D.MipLevels = 1;
        hr = m_device->CreateShaderResourceView(m_captureTexture.Get(), &ySrv, &m_captureSRV);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC uvSrv{};
        uvSrv.Format = DXGI_FORMAT_R8G8_UNORM;
        uvSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvSrv.Texture2D.MipLevels = 1;
        hr = m_device->CreateShaderResourceView(m_captureTexture.Get(), &uvSrv, &m_captureSRV_UV);
        if (FAILED(hr)) return false;

        m_captureFormat = CaptureFormatKind::NV12;
    } else {
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width     = width;
        texDesc.Height    = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc = {1, 0};
        texDesc.Usage     = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_captureTexture);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = m_device->CreateShaderResourceView(m_captureTexture.Get(), &srvDesc, &m_captureSRV);
        if (FAILED(hr)) return false;

        m_captureFormat = CaptureFormatKind::BGRA;
    }

    m_captureWidth  = width;
    m_captureHeight = height;
    return true;
}

bool DX11Renderer::CreateCaptureResourcesP010(uint32_t width, uint32_t height)
{
    ForgetResyncFrame(CaptureFrameHoldReason::ResourcesRecreated);
    // P010 capture texture. The DXGI format is planar 16-bit-per-channel,
    // with the top 10 bits of each 16-bit value carrying the data and the
    // bottom 6 bits zero. Two SRVs are bound into the same underlying texture:
    //   - Y plane viewed as R16_UNORM (luma)
    //   - UV plane viewed as R16G16_UNORM (chroma, half resolution)
    // The shader uses bilinear sampling on both; YUV to RGB conversion plus
    // BT.2020 PQ inverse EOTF happen per-pixel in g_pixelShaderP010.
    //
    // P010 has stricter constraints than NV12:
    //   - width must be even (chroma is half-res)
    //   - height must be even (same reason)
    //   - some drivers/feature levels don't accept D3D11_USAGE_DYNAMIC for
    //     P010: if that happens, the failure is logged below and a
    //     STAGING+CopyResource pattern would be the next fix.
    m_captureTexture.Reset();
    m_captureSRV.Reset();
    m_captureSRV_UV.Reset();

    // P010 requires even dimensions for the 4:2:0 chroma plane.
    if ((width & 1) || (height & 1)) {
        OutputDebugStringW(L"[NitLink/Renderer] P010 requires even dimensions; declining\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width     = width;
    texDesc.Height    = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format    = DXGI_FORMAT_P010;
    texDesc.SampleDesc = {1, 0};
    texDesc.Usage     = D3D11_USAGE_DYNAMIC;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_captureTexture);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] CreateTexture2D(P010, DYNAMIC) failed hr=0x"
           << std::hex << hr << L"\n";
        OutputDebugStringW(ss.str().c_str());

        // Retry with USAGE_DEFAULT + UpdateSubresource model? Some Windows
        // builds reject DYNAMIC on planar 10-bit formats. That path isn't
        // wired yet: log and return false; UpdateCaptureTexture will see
        // null texture and skip the frame.
        m_captureTexture.Reset();
        return false;
    }

    OutputDebugStringW(L"[NitLink/Renderer] P010 texture created OK\n");

    // Y plane SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC ySrv{};
    ySrv.Format = DXGI_FORMAT_R16_UNORM;
    ySrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrv.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_captureTexture.Get(), &ySrv, &m_captureSRV);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] CreateSRV(P010 Y as R16_UNORM) failed hr=0x"
           << std::hex << hr << L"\n";
        OutputDebugStringW(ss.str().c_str());
        // Clean up so the caller's early-return check catches this. Without
        // it, the texture lingers while m_captureFormat stays BGRA, and
        // the next memcpy interprets P010 bytes as BGRA: boom.
        m_captureTexture.Reset();
        m_captureSRV.Reset();
        m_captureSRV_UV.Reset();
        return false;
    }

    OutputDebugStringW(L"[NitLink/Renderer] P010 Y-plane SRV created OK\n");

    // UV plane SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC uvSrv{};
    uvSrv.Format = DXGI_FORMAT_R16G16_UNORM;
    uvSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvSrv.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_captureTexture.Get(), &uvSrv, &m_captureSRV_UV);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] CreateSRV(P010 UV as R16G16_UNORM) failed hr=0x"
           << std::hex << hr << L"\n";
        OutputDebugStringW(ss.str().c_str());
        // Same cleanup as Y SRV path.
        m_captureTexture.Reset();
        m_captureSRV.Reset();
        m_captureSRV_UV.Reset();
        return false;
    }

    OutputDebugStringW(L"[NitLink/Renderer] P010 UV-plane SRV created OK\n");

    m_captureWidth  = width;
    m_captureHeight = height;
    m_captureFormat = CaptureFormatKind::P010;

    std::wstringstream ss;
    ss << L"[NitLink/Renderer] P010 capture texture ready: " << width << L"x" << height << L"\n";
    OutputDebugStringW(ss.str().c_str());
    return true;
}

void DX11Renderer::UpdateCaptureTexture(const uint8_t* data, uint32_t size, uint32_t width, uint32_t height)
{
    const auto phaseStart = std::chrono::steady_clock::now();
    // Trust the format the application declared via SetSourceFormat: the
    // application got the authoritative subtype GUID back from CaptureDevice's
    // MF negotiation. The previous design here inferred format from the byte
    // count, which silently picked the wrong format whenever a partial frame
    // arrived during an HDMI signal-loss window (PS5 black-frame transitions
    // produced "almost the right size" frames that landed in an adjacent
    // format's tolerance band: uploaded as the wrong format -> green frame
    // until the next clean frame got things back in sync).
    //
    // If SetSourceFormat hasn't been called yet, drop the frame. The
    // application sequences SetSourceFormat before StartCapture so this
    // should only fire in pathological startup orderings (e.g. an early
    // pre-init test path).
    if (!m_sourceFormatSet) {
        OutputDebugStringW(L"[NitLink/Renderer] UpdateCaptureTexture: source format not declared yet, dropping frame\n");
        return;
    }

    // Validate the byte count against the declared format's complete packed
    // image. MF's ConvertToContiguousBuffer combines sample buffers into one
    // allocation, or returns the existing buffer when there is only one; it
    // does not promise that every driver supplies an exact packed byte count.
    // Alignment padding can add unused bytes after pixel rows, so contiguity
    // alone is not evidence that the sample length must match exactly.
    // The upload below retains the declared packed row/plane layout: this
    // size guard does not infer another pixel format or an arbitrary stride.
    // Reference: https://learn.microsoft.com/en-us/windows/win32/medfound/image-stride
    uint32_t expectedSize = 0, rowBytes = 0;
    PixelLayout layout;
    switch (m_sourceFormat) {
        case CaptureFormatKind::BGRA: layout = PixelLayout::Bgra; break;
        case CaptureFormatKind::NV12: layout = PixelLayout::Nv12; break;
        case CaptureFormatKind::P010: layout = PixelLayout::P010; break;
        default: return;
    }
    if (!data || !FrameLayout(width, height, layout, rowBytes, expectedSize)) return;
    // Permit surplus bytes up to 5% of the packed size, with a 1 KiB floor.
    // A percentage scales the allowance with resolution instead of imposing
    // one small fixed limit on every frame; the floor leaves alignment slack
    // for small images whose percentage alone would be only a few bytes.
    // Padding can increase a buffer's length, never replace missing pixels,
    // so the allowance applies only above expectedSize.
    //
    // Reject a short sample outright rather than clamping a partial copy.
    // FrameBuffer::Write overwrites only the delivered bytes of a reused slot;
    // its unwritten tail can still contain pixels from a previous frame.
    // Treating that prefix as a complete image would expose the stale tail,
    // while shortening the upload would leave part of the image unwritten.
    // Rejection happens before Map, preserving the last complete capture
    // texture until a full sample arrives, including across HDMI signal gaps.
    const uint32_t tolerance = (std::max)(expectedSize / 20, 1024u);
    if (expectedSize == 0 ||
        size < expectedSize ||
        size > expectedSize + tolerance) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] UpdateCaptureTexture: size mismatch (got "
           << size << L", expected " << expectedSize << L" to " << expectedSize + tolerance
           << L"), dropping partial/transient frame\n";
        OutputDebugStringW(ss.str().c_str());
        return;
    }

    // Allocate / reallocate GPU resources to match the declared format and
    // current dimensions. m_captureFormat tracks what's currently allocated;
    // when it diverges from m_sourceFormat (declared), tear down and rebuild.
    // CreateCaptureResources(_P010) updates m_captureFormat at the end so
    // subsequent calls hit the no-op path.
    if (width != m_captureWidth ||
        height != m_captureHeight ||
        m_sourceFormat != m_captureFormat) {
        if (m_sourceFormat == CaptureFormatKind::P010) {
            CreateCaptureResourcesP010(width, height);
        } else {
            CreateCaptureResources(width, height,
                                    m_sourceFormat == CaptureFormatKind::NV12);
        }
    }

    if (!m_captureTexture) return;

    // Defensive: CreateCaptureResources* may have failed silently and left
    // m_captureFormat still mismatched against m_sourceFormat. Uploading
    // bytes through the wrong format's branch would memcpy past the source
    // buffer (e.g. BGRA's 4x stride against a 1.5x NV12 buffer). Drop the
    // frame in that case rather than risk a heap stomp.
    if (m_sourceFormat != m_captureFormat || width != m_captureWidth || height != m_captureHeight) {
        OutputDebugStringW(L"[NitLink/Renderer] UpdateCaptureTexture: resource allocation lagging declared format, dropping frame\n");
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_captureTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        // The capture upload Map is the other per-frame device call besides
        // Present, so device loss can surface here first. Latch it for the run
        // loop rather than silently dropping every frame from now on.
        FlagIfDeviceLost(hr, L"UpdateCaptureTexture Map");
        return;
    }

    // WRITE_DISCARD has already replaced the old contents, even if validation
    // below rejects this mapped buffer. Do not hold an unpresented/partial write.
    m_captureFrameHold.Invalidate(CaptureFrameHoldReason::TextureOverwritten);
    if (!mapped.pData || mapped.RowPitch < rowBytes) {
        m_context->Unmap(m_captureTexture.Get(), 0);
        return;
    }

    if (m_captureFormat == CaptureFormatKind::NV12) {
        // NV12 source layout: Y plane (width * height bytes), then UV (width * height/2 bytes interleaved)
        // GPU texture layout: same, but with RowPitch stride
        uint8_t*       dst   = static_cast<uint8_t*>(mapped.pData);
        const uint8_t* srcY  = data;
        const uint8_t* srcUV = data + (width * height);

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst + size_t(y) * mapped.RowPitch, srcY + y * width, width);
        }

        uint8_t* dstUV = dst + (size_t(mapped.RowPitch) * height);
        for (uint32_t y = 0; y < height / 2; y++) {
            memcpy(dstUV + size_t(y) * mapped.RowPitch, srcUV + y * width, width);
        }
    } else if (m_captureFormat == CaptureFormatKind::P010) {
        // P010 source layout: Y plane is width*height 16-bit values (so
        // width*height*2 bytes; data carried in top 10 bits, bottom 6
        // zero), then UV plane is half-res interleaved 16-bit pairs
        // (width*height/2 pairs = width*height*2 bytes UV total: same
        // size as Y plane).
        //
        // GPU texture (DXGI_FORMAT_P010) expects the same layout but with
        // mapped.RowPitch stride between Y plane rows. UV plane starts at
        // dst + RowPitch * height (matching D3D11 P010 spec).
        const uint32_t yRowBytes  = width * 2;            // 16-bit per pixel
        const uint32_t uvRowBytes = width * 2;            // half-width pairs, each pair 4 bytes? No: see below.
        // Actually for interleaved UV at 4:2:0:
        //   - one chroma sample per 2x2 luma block
        //   - U and V interleaved -> 2 chroma bytes per 2 luma samples horizontally
        //   - At 16 bpc that's 4 bytes per 2 luma samples = 2 bytes per luma sample
        // -> uvRowBytes == yRowBytes, but only height/2 rows total.

        uint8_t*       dst   = static_cast<uint8_t*>(mapped.pData);
        const uint8_t* srcY  = data;
        const uint8_t* srcUV = data + (yRowBytes * height);

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst + size_t(y) * mapped.RowPitch, srcY + y * yRowBytes, yRowBytes);
        }

        uint8_t* dstUV = dst + (size_t(mapped.RowPitch) * height);
        for (uint32_t y = 0; y < height / 2; y++) {
            memcpy(dstUV + size_t(y) * mapped.RowPitch, srcUV + y * uvRowBytes, uvRowBytes);
        }
    } else {
        // BGRA
        uint32_t srcPitch = width * 4;
        const uint8_t* src = data;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst, src, srcPitch);
            src += srcPitch;
            dst += mapped.RowPitch;
        }
    }

    m_context->Unmap(m_captureTexture.Get(), 0);
    m_phaseUploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phaseStart).count();
    m_hasFrame = true;
    ++m_captureUploadSerial;
    m_captureFrameHold.ObserveCompletedUpload(m_captureUploadSerial);
}

void DX11Renderer::Resize(uint32_t width, uint32_t height)
{
    if (width < 8 || height < 8) return;
    if (width == m_windowWidth && height == m_windowHeight) return;

    {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] Resize " << m_windowWidth << L"x" << m_windowHeight
           << L" -> " << width << L"x" << height << L"\n";
        OutputDebugStringW(ss.str().c_str());
    }

    m_windowWidth = width;
    m_windowHeight = height;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->Flush();
    OutputDebugStringW(L"[NitLink/Renderer] Resize: flushed GPU\n");

    ReleaseRenderTarget();
    OutputDebugStringW(L"[NitLink/Renderer] Resize: released RTV\n");

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] ResizeBuffers FAILED 0x" << std::hex << hr << L"\n";
        OutputDebugStringW(ss.str().c_str());
        // A device removed during resize (TDR, driver upgrade, sleep/resume)
        // surfaces here rather than at Present. Route it into the same latch
        // the run loop polls so recovery engages instead of leaving the swap
        // chain at its old size with no rebuild ever triggered.
        FlagIfDeviceLost(hr, L"Resize ResizeBuffers");
        return;
    }
    OutputDebugStringW(L"[NitLink/Renderer] Resize: swap chain resized\n");

    CreateRenderTarget();
    OutputDebugStringW(L"[NitLink/Renderer] Resize: RTV recreated, done\n");
}

bool DX11Renderer::UpdateAspectTransform()
{
    if (!m_captureWidth || !m_captureHeight || !m_windowWidth || !m_windowHeight) return false;

    float captureAspect = (float)m_captureWidth  / (float)m_captureHeight;
    float windowAspect  = (float)m_windowWidth   / (float)m_windowHeight;
    if (m_aspectOverride > 0.0f) captureAspect = m_aspectOverride;
    if (m_aspectOverride < 0.0f) captureAspect = windowAspect;

    TransformCB cb{};
    if (windowAspect > captureAspect) {
        // Window is wider than capture: pillarbox (black bars on left/right)
        cb.scaleX = captureAspect / windowAspect;
        cb.scaleY = 1.0f;
    } else {
        // Window is taller than capture: letterbox (black bars on top/bottom)
        cb.scaleX = 1.0f;
        cb.scaleY = windowAspect / captureAspect;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_transformCB.Get(), 0);
        return true;
    }
    return false;
}

bool DX11Renderer::CreateGpuTimingQueries()
{
    D3D11_QUERY_DESC disjointDesc{};
    disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC tsDesc{};
    tsDesc.Query = D3D11_QUERY_TIMESTAMP;

    for (int i = 0; i < kGpuQueryRingSize; ++i) {
        HRESULT hr = m_device->CreateQuery(&disjointDesc, &m_gpuQueryDisjoint[i]);
        if (FAILED(hr)) return false;
        hr = m_device->CreateQuery(&tsDesc, &m_gpuQueryStart[i]);
        if (FAILED(hr)) return false;
        hr = m_device->CreateQuery(&tsDesc, &m_gpuQueryEnd[i]);
        if (FAILED(hr)) return false;
    }
    return true;
}

bool DX11Renderer::SetPresentCap(double hz)
{
    if (m_presentCapFromMarker) return false;
    m_vrrCapHz = (hz > 0.0) ? hz : 0.0;
    return true;
}

void DX11Renderer::SetVSync(bool on)
{
    m_vsync = on;
    OutputDebugStringW(on
        ? L"[NitLink/Renderer] VSync ON: Present(1, 0); tearing cap bypassed\n"
        : L"[NitLink/Renderer] VSync OFF: Present(0, ALLOW_TEARING)\n");
}

int DX11Renderer::PresentationMode() const
{
    ComPtr<IDXGISwapChainMedia> media;
    if (!m_swapChain || FAILED(m_swapChain.As(&media))) return -1;
    DXGI_FRAME_STATISTICS_MEDIA stats{};
    if (FAILED(media->GetFrameStatisticsMedia(&stats))) return -1;
    return static_cast<int>(stats.CompositionMode);
}

DX11Renderer::PhaseTimes DX11Renderer::ConsumePhaseTimes()
{
    PhaseTimes t{};
    if (m_phaseWaits > 0) {
        t.waitMs   = m_phaseWaitMs   / m_phaseWaits;
        t.uploadMs = m_phaseUploadMs / m_phaseWaits;
    }
    if (m_phasePresents > 0) {
        t.presentMs = m_phasePresentMs / m_phasePresents;
    }
    t.iterations = m_phaseWaits;
    t.presents   = m_phasePresents;
    m_phaseWaitMs = m_phaseUploadMs = m_phasePresentMs = 0.0;
    m_phaseWaits = m_phasePresents = 0;
    return t;
}

void DX11Renderer::WaitForFrameReady()
{
    const auto phaseStart = std::chrono::steady_clock::now();
    m_phaseWaits++;
    // The DXGI 1.3 low-latency wait, callable on its own so the Low-Latency
    // loop can wait at the TOP of the iteration, then read the freshest capture
    // frame, then call BeginFrame(false) (the wait has already happened).
    // VRR cap (VRR_CAP.txt): pace to ~m_vrrCapHz instead of the swap-chain waitable,
    // so the ALLOW_TEARING present stays under the VRR ceiling and engages VRR. The
    // capture frame is read right after this returns, so it stays fresh.
    // Sync already schedules presentation; an additional cap can miss a
    // refresh boundary. This also bypasses diagnostic marker caps under VSync.
    if (!m_vsync && m_vrrCapHz > 0.0) {
        const auto interval = std::chrono::nanoseconds((long long)(1.0e9 / m_vrrCapHz));
        const auto target   = m_lastPresentTime + interval;
        while (std::chrono::steady_clock::now() < target) {
            const auto remain = target - std::chrono::steady_clock::now();
            if (remain > std::chrono::milliseconds(2))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else
                std::this_thread::yield();
        }
        m_phaseWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phaseStart).count();
        return;
    }
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
    }
    m_phaseWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phaseStart).count();
}

void DX11Renderer::BeginFrame(bool doWait)
{
    m_captureDrawnThisFrame = false;
    m_heldCaptureDrawnThisFrame = false;
    m_liveCaptureDrawnThisFrame = false;
    m_upscaledCaptureCompositedThisFrame = false;
    m_lastPresentSucceeded = false;
    // ============== DXGI 1.3 LOW-LATENCY WAIT ==============
    // Block until DXGI says "ready for your next frame." If the previous
    // Present is still working its way to the display, this wait holds the
    // render thread back so it DOES NOT pile up a queue of stale frames.
    // The waitable object is signaled the moment DXGI finishes presenting
    // the previous frame.
    //
    // Timeout is 1000ms: should never actually fire, but a finite value
    // prevents a broken signal from deadlocking the render thread forever.
    // bAlertable = TRUE so the WaitForSingleObjectEx can be unblocked by
    // a Windows APC if needed (rare but cheap insurance).
    //
    // First-frame note: Microsoft's docs are explicit that the wait must
    // happen BEFORE the first Present too, otherwise the queue starts at
    // +1 and the latency saving is never recovered. Since BeginFrame runs
    // before EndFrame's Present in every iteration including the first,
    // the ordering is already correct here. Skipped when doWait==false: the
    // Low-Latency loop already waited via WaitForFrameReady() at the top.
    if (doWait && m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
    }

    // Frame timer for the averaged "render ms" telemetry. Measured from
    // here (just after the latency wait, the actual *start* of useful work)
    // until the Present completes at the bottom of EndFrame.
    QueryPerformanceCounter(&m_frameStartQpc);

    // GPU timing readback: pull frame N-3's queries if the ring has filled.
    // Reading at frame N+3 means the GPU has long since finished the work
    // being measured, so GetData returns S_OK without forcing a flush.
    if (m_gpuQueryEnabled
        && m_gpuQueryFrameCount >= kGpuQueryRingSize
        && m_gpuQueryIssued[m_gpuQueryWriteSlot])
    {
        const int readSlot = m_gpuQueryWriteSlot;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 startTs = 0, endTs = 0;
        HRESULT hrDj    = m_context->GetData(m_gpuQueryDisjoint[readSlot].Get(),
                                              &dj, sizeof(dj), 0);
        HRESULT hrStart = m_context->GetData(m_gpuQueryStart[readSlot].Get(),
                                              &startTs, sizeof(startTs), 0);
        HRESULT hrEnd   = m_context->GetData(m_gpuQueryEnd[readSlot].Get(),
                                              &endTs, sizeof(endTs), 0);
        if (hrDj == S_OK && hrStart == S_OK && hrEnd == S_OK
            && !dj.Disjoint && dj.Frequency != 0
            && endTs >= startTs)
        {
            m_lastGpuMs = (double)(endTs - startTs) * 1000.0 / (double)dj.Frequency;
        }
        // The disjoint flag set means the GPU clock disconnected during the
        // measurement window (power-state change, etc). Skip the update;
        // m_lastGpuMs retains its previous value rather than reading garbage.
        m_gpuQueryIssued[readSlot] = false;
    }

    // Open this frame's GPU timing window. The disjoint query brackets the
    // pair of timestamp queries; only the disjoint takes Begin/End, the
    // timestamps only take End (they fire at the moment End is recorded
    // onto the command stream).
    m_gpuQueryActiveThisFrame = false;
    if (m_gpuQueryEnabled) {
        m_context->Begin(m_gpuQueryDisjoint[m_gpuQueryWriteSlot].Get());
        m_context->End  (m_gpuQueryStart   [m_gpuQueryWriteSlot].Get());
        m_gpuQueryActiveThisFrame = true;
    }

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    // Always clear the backbuffer (the overlay + final composite live here).
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);

    // If upscaling, also clear the intermediate that DrawCaptureFrame writes
    // to. The intermediate is sized to capture resolution (not window) so
    // NIS sees a clean native-res frame without letterboxing baked in.
    if (m_postInputEnabled && m_captureWidth && m_captureHeight) {
        if (m_postInputW != m_captureWidth || m_postInputH != m_captureHeight) {
            CreatePostInputTarget(m_captureWidth, m_captureHeight);
        }
        if (m_postInputRTV) {
            m_context->ClearRenderTargetView(m_postInputRTV.Get(), clearColor);
        }
    }

    D3D11_VIEWPORT vp{};
    vp.Width    = (float)m_windowWidth;
    vp.Height   = (float)m_windowHeight;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

CaptureFrameSemantics DX11Renderer::CurrentCaptureFrameSemantics() const
{
    return {m_captureWidth, m_captureHeight, static_cast<std::uint32_t>(m_sourceFormat),
            m_windowWidth, m_windowHeight, static_cast<std::uint32_t>(m_currentSwapFormat),
            m_sourceIsHDR10, m_sourceTopDown, m_sourceFullRange, m_p010LimitedChroma,
            m_hdrEnabled, m_hdrUseBT709Matrix, m_postInputEnabled,
            m_hdrEnabled ? 0.0f : m_colorExpansionCurrent, m_colorExpansionTarget, m_aspectOverride};
}

CaptureFrameHoldStatus DX11Renderer::GetResyncFrameStatus() const
{
    const bool planar = m_captureFormat != CaptureFormatKind::BGRA;
    const bool shaderReady = m_captureFormat == CaptureFormatKind::P010 ? !!m_pixelShaderP010 :
        m_captureFormat == CaptureFormatKind::NV12 ? !!m_pixelShaderNV12 : !!m_pixelShaderBGRA;
    const bool resourcesReady = !m_deviceLost && m_hasFrame && m_sourceFormatSet &&
        m_sourceFormat == m_captureFormat &&
        m_device && m_context && m_rtv && m_captureTexture && m_captureSRV &&
        (!planar || m_captureSRV_UV) && shaderReady && m_vertexShader && m_vertexBuffer &&
        m_inputLayout && m_pixelCB && m_transformCB && m_sampler &&
        m_captureWidth && m_captureHeight && m_windowWidth && m_windowHeight &&
        (!m_postInputEnabled || (m_postInputRTV && m_postInputSRV && m_compositePS));
    return m_captureFrameHold.Inspect(
        resourcesReady, m_captureUploadSerial, CurrentCaptureFrameSemantics());
}

void DX11Renderer::ForgetResyncFrame(CaptureFrameHoldReason reason)
{
    m_captureFrameHold.Invalidate(reason);
}

void DX11Renderer::InvalidateCaptureFrame()
{
    m_hasFrame = false;
    ForgetResyncFrame(CaptureFrameHoldReason::CaptureSessionInvalidated);
}

bool DX11Renderer::DrawCaptureFrame(bool heldForSignalResync)
{
    if (!m_hasFrame || !m_captureSRV) return false;
    if (heldForSignalResync && !GetResyncFrameStatus().compatible) return false;

    // Smoothly approach the target color-expansion value
    // A held raw frame uses its last presented conversion, without advancing
    // the live-frame animation or changing the user's target/pipeline policy.
    if (!heldForSignalResync)
        m_colorExpansionCurrent += (m_colorExpansionTarget - m_colorExpansionCurrent) * 0.22f;

    // When HDR is on, force color-expansion off: limited-range expansion
    // would skew the sRGB-to-linear decode.
    const float effectiveColorExp = m_hdrEnabled ? 0.0f : m_colorExpansionCurrent;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool captureConstantsUpdated = false;
    if (SUCCEEDED(m_context->Map(m_pixelCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* data = static_cast<float*>(mapped.pData);
        data[0] = effectiveColorExp;
        data[1] = m_hdrEnabled ? 1.0f : 0.0f;   // hdrMode (1 = HDR10 PQ output)
        // Slot 2: P010 shader uses this as the "swap chain is BT.709 fallback"
        // flag. Other shaders treat slot 2 as unused. Setting it from
        // m_hdrUseBT709Matrix here means the P010 shader applies the
        // BT.2020-to-BT.709 rotation when DXGI couldn't provide a BT.2020 chain.
        data[2] = m_hdrUseBT709Matrix ? 1.0f : 0.0f;
        // Slot 3: BGRA + NV12 shaders use this to decide whether to flip V
        // back (top-down source) or pass V through (bottom-up source).
        // Set by SetSourceRowOrder() from CaptureFormat::topDown.
        data[3] = m_sourceTopDown ? 1.0f : 0.0f;
        // Slot 4: BGRA + NV12 shaders use this to decide whether to apply
        // the limited→full range expansion. Most HDMI sources come in as
        // limited range (16-235) and need it; some driver paths (notably
        // 4K S NV12) convert to full range (0-255) before delivering, in
        // which case applying it would crush blacks. Set by
        // SetSourceFullRange() from CaptureFormat::fullRange.
        data[4] = m_sourceFullRange ? 1.0f : 0.0f;
        // Slot 5: P010 shader's "tonemap HDR10 source down to SDR backbuffer"
        // flag. True when source is HDR10 AND user wants SDR output (m_hdrEnabled
        // false). In that case the shader does PQ inverse EOTF → linear BT.2020
        // → BT.709 matrix → Reinhard tonemap → sRGB encode, writing 8-bit
        // limited-range RGB to the SDR backbuffer. False in every other case
        // (HDR path, plain SDR via NV12, etc).
        const bool sdrFromHdrTonemap = m_sourceIsHDR10 && !m_hdrEnabled;
        data[5] = sdrFromHdrTonemap ? 1.0f : 0.0f;
        // Slot 6 is P010-only; reset explicitly when the source policy changes.
        data[6] = m_p010LimitedChroma ? 1.0f : 0.0f;
        data[7] = 0.0f;
        m_context->Unmap(m_pixelCB.Get(), 0);
        captureConstantsUpdated = true;
    }
    if (heldForSignalResync && !captureConstantsUpdated) {
        ForgetResyncFrame(CaptureFrameHoldReason::ResourceUnavailable);
        return false;
    }

    // ---- Pick target + transform ----
    // Normal path: draw directly to backbuffer with aspect letterboxing.
    // Upscale path: draw to intermediate at capture-native resolution with
    // identity transform (NIS gets a clean source).
    bool transformUpdated = false;
    if (m_postInputEnabled && m_postInputRTV) {
        TransformCB identity{ 1.0f, 1.0f };
        D3D11_MAPPED_SUBRESOURCE tm;
        if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &tm))) {
            memcpy(tm.pData, &identity, sizeof(identity));
            m_context->Unmap(m_transformCB.Get(), 0);
            transformUpdated = true;
        }
        m_context->OMSetRenderTargets(1, m_postInputRTV.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width    = (float)m_postInputW;
        vp.Height   = (float)m_postInputH;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);
    } else {
        transformUpdated = UpdateAspectTransform();
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width    = (float)m_windowWidth;
        vp.Height   = (float)m_windowHeight;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);
    }

    if (heldForSignalResync && !transformUpdated) {
        ForgetResyncFrame(CaptureFrameHoldReason::ResourceUnavailable);
        return false;
    }
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_transformCB.GetAddressOf());

    switch (m_captureFormat) {
    case CaptureFormatKind::P010: {
        m_context->PSSetShader(m_pixelShaderP010.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_captureSRV.Get(), m_captureSRV_UV.Get() };
        m_context->PSSetShaderResources(0, 2, srvs);
        break;
    }
    case CaptureFormatKind::NV12: {
        m_context->PSSetShader(m_pixelShaderNV12.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_captureSRV.Get(), m_captureSRV_UV.Get() };
        m_context->PSSetShaderResources(0, 2, srvs);
        break;
    }
    case CaptureFormatKind::BGRA:
    default:
        m_context->PSSetShader(m_pixelShaderBGRA.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, m_captureSRV.GetAddressOf());
        break;
    }
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_pixelCB.GetAddressOf());

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_context->Draw(4, 0);
    m_captureDrawnThisFrame = captureConstantsUpdated && transformUpdated;
    m_heldCaptureDrawnThisFrame = heldForSignalResync && m_captureDrawnThisFrame;
    m_liveCaptureDrawnThisFrame = !heldForSignalResync && m_captureDrawnThisFrame;
    m_drawnCaptureSemantics = CurrentCaptureFrameSemantics();
    m_drawnCaptureUploadSerial = m_captureUploadSerial;
    return true;
}

// Composite the upscaler's output onto the backbuffer using the normal aspect
// transform. This is called after the upscaler's Dispatch(). srcW/srcH are
// the NIS output dimensions, used to update the aspect math.
void DX11Renderer::CompositeUpscaledTexture(ID3D11ShaderResourceView* upscaledSRV,
                                              uint32_t srcW, uint32_t srcH)
{
    if (!upscaledSRV || !m_compositePS) return;

    bool transformUpdated = false;
    // Aspect transform: pretend the "capture" is now the upscaled output.
    if (srcW && srcH && m_windowWidth && m_windowHeight) {
        float srcAspect = (float)srcW / (float)srcH;
        float winAspect = (float)m_windowWidth / (float)m_windowHeight;
        if (m_aspectOverride > 0.0f) srcAspect = m_aspectOverride;
        if (m_aspectOverride < 0.0f) srcAspect = winAspect;
        TransformCB cb{};
        if (winAspect > srcAspect) {
            cb.scaleX = srcAspect / winAspect;
            cb.scaleY = 1.0f;
        } else {
            cb.scaleX = 1.0f;
            cb.scaleY = winAspect / srcAspect;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &cb, sizeof(cb));
            m_context->Unmap(m_transformCB.Get(), 0);
            transformUpdated = true;
        }
    }
    if (m_heldCaptureDrawnThisFrame && !transformUpdated) {
        ForgetResyncFrame(CaptureFrameHoldReason::ResourceUnavailable);
        return;
    }

    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (float)m_windowWidth;
    vp.Height   = (float)m_windowHeight;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_transformCB.GetAddressOf());
    m_context->PSSetShader(m_compositePS.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &upscaledSRV);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_context->Draw(4, 0);

    // Unbind so the next frame doesn't complain about input/output coupling.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    m_upscaledCaptureCompositedThisFrame = transformUpdated;
}

// HDR overlay compositor. Draws a fullscreen quad textured with the supplied
// BGRA8 (sRGB) UI render. The pixel shader converts sRGB to scRGB and applies
// a ~200-nit brightness scale so the SDR-authored overlay text appears at the
// correct luminance against HDR content.
//
// Alpha compositing: enable straight alpha blend so the transparent pixels
// of the UI texture (everywhere except the actual overlay text/box) show
// the previously-rendered backbuffer content (the capture frame).
void DX11Renderer::CompositeUI(ID3D11ShaderResourceView* uiSRV)
{
    if (!uiSRV || !m_pixelShaderUI) return;

    // Identity transform: UI texture is window-sized, blit 1:1.
    TransformCB cb{};
    cb.scaleX = 1.0f;
    cb.scaleY = 1.0f;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        m_context->Unmap(m_transformCB.Get(), 0);
    }

    // Backbuffer (the FP16 in HDR mode, BGRA8 in SDR).
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (float)m_windowWidth;
    vp.Height   = (float)m_windowHeight;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    // Alpha blend state: standard SrcAlpha / (1-SrcAlpha). The UI texture
    // has transparent pixels everywhere there's no text drawn, so the
    // composite passes through to whatever was on the backbuffer beneath.
    // Cache the blend state on first use to avoid recreating per-frame.
    if (!m_uiBlendState) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bd, &m_uiBlendState);
    }
    const float blendFactor[4] = { 0, 0, 0, 0 };
    m_context->OMSetBlendState(m_uiBlendState.Get(), blendFactor, 0xFFFFFFFF);

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_transformCB.GetAddressOf());
    m_context->PSSetShader(m_pixelShaderUI.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &uiSRV);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_context->Draw(4, 0);

    // Restore default no-blend state for subsequent draws this frame.
    m_context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

    ID3D11ShaderResourceView* nullSRV2 = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV2);
}

// Format an HRESULT as "0x" + 8 uppercase hex digits, most-significant first.
// Device-removed reasons (e.g. 0x887A0006 DXGI_ERROR_DEVICE_HUNG) only read
// sensibly in hex.
static std::wstring HrToHex(HRESULT hr)
{
    const wchar_t* digits = L"0123456789ABCDEF";
    unsigned v = static_cast<unsigned>(hr);
    std::wstring s = L"0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        s += digits[(v >> shift) & 0xF];
    }
    return s;
}

void DX11Renderer::FlagIfDeviceLost(HRESULT hr, const wchar_t* site)
{
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) return;

    // GetDeviceRemovedReason gives the specific cause (hung, reset, driver
    // upgrade, out-of-memory); the Present/Map HRESULT only says "device gone".
    HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
    OutputDebugStringW((L"[NitLink/Renderer] graphics device lost at " + std::wstring(site)
        + L" (hr=" + HrToHex(hr) + L", reason=" + HrToHex(reason)
        + L"); flagging renderer rebuild\n").c_str());
    m_deviceLost = true;
}

bool DX11Renderer::ConsumeDeviceLost()
{
    // Present latches most losses, but a reset that lands between two
    // presents leaves a whole iteration of capture upload, frame differ, and
    // overlay work running against a removed device. Ask the device directly
    // before the iteration starts, so that work is skipped and the rebuild
    // begins at once.
    if (!m_deviceLost && m_device) {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        if (FAILED(reason)) {
            OutputDebugStringW((L"[NitLink/Renderer] graphics device lost (reason=" + HrToHex(reason)
                + L"); flagging renderer rebuild\n").c_str());
            m_deviceLost = true;
        }
    }
    bool v = m_deviceLost;
    m_deviceLost = false;
    return v;
}

void DX11Renderer::EndFrame()
{
    // Close this frame's GPU timing window BEFORE Present, so the measured
    // span covers only the work issued between BeginFrame and here. Present
    // itself is a mix of GPU and CPU/driver work and is not the per-frame
    // shader cost being measured.
    if (m_gpuQueryActiveThisFrame) {
        m_context->End(m_gpuQueryEnd     [m_gpuQueryWriteSlot].Get());
        m_context->End(m_gpuQueryDisjoint[m_gpuQueryWriteSlot].Get());
        m_gpuQueryIssued[m_gpuQueryWriteSlot] = true;
        m_gpuQueryActiveThisFrame             = false;
        m_gpuQueryWriteSlot = (m_gpuQueryWriteSlot + 1) % kGpuQueryRingSize;
        if (m_gpuQueryFrameCount < kGpuQueryRingSize) m_gpuQueryFrameCount++;
    }

    const auto presentStart = std::chrono::steady_clock::now();
    HRESULT hrPresent = S_OK;
    if (m_vsync) {
        // ALLOW_TEARING is invalid with a nonzero sync interval.
        hrPresent = m_swapChain->Present(1, 0);
    } else {
        // Immediate present, tearing allowed: lowest latency, relies on the
        // display's VRR to avoid tearing within its refresh range.
        hrPresent = m_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    }
    // Present is the per-frame sentinel for device loss: a TDR or driver
    // upgrade surfaces here as DXGI_ERROR_DEVICE_REMOVED/_RESET. Latch it for
    // the run loop instead of presenting into the void forever.
    FlagIfDeviceLost(hrPresent, L"Present");
    // S_OK, rather than SUCCEEDED, excludes DXGI_STATUS_OCCLUDED. Cached
    // resync draws/settings/overlays cannot create a new live-frame receipt.
    m_lastPresentSucceeded = hrPresent == S_OK;
    m_captureFrameHold.ObservePresent(
        m_liveCaptureDrawnThisFrame,
        !m_drawnCaptureSemantics.postInput || m_upscaledCaptureCompositedThisFrame,
        m_lastPresentSucceeded, m_drawnCaptureUploadSerial, m_drawnCaptureSemantics);
    m_phasePresentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
    m_phasePresents++;
    // Timestamp the present so the VRR present-rate cap (WaitForFrameReady) can pace
    // the next iteration just under the display's VRR ceiling.
    m_lastPresentTime = std::chrono::steady_clock::now();

    // ============== FRAME-TIME TELEMETRY ==============
    // Measure BeginFrame -> Present-returned time and roll it into a sliding
    // average. This is NitLink's end of the pipeline: how long it takes to
    // turn an already-arrived capture frame into a presented backbuffer.
    //
    // It's NOT the full system latency (which also includes the capture card,
    // Media Foundation, GPU upload time, and the display's own delay), but
    // it's the part of latency under direct control here, and it's the right
    // number to optimize against. Total system latency is measured externally
    // with a 240fps camera and a button-press / on-screen-response timing test.
    //
    // Average over ~60 frames (~1 second at 60fps) before publishing to
    // m_lastReportedRenderMs. A sliding window keeps the UI from jittering
    // every frame while still being responsive to genuine perf changes.
    if (m_frameStartQpc.QuadPart) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double ms = (now.QuadPart - m_frameStartQpc.QuadPart)
                        * 1000.0 / static_cast<double>(freq.QuadPart);
        m_renderMsSum  += ms;
        m_renderMsCount += 1;
        if (m_renderMsCount >= 60) {
            m_lastReportedRenderMs = m_renderMsSum / m_renderMsCount;
            m_renderMsSum  = 0.0;
            m_renderMsCount = 0;
        }
    }
}

// HDR10 screenshot tonemap: convert one R10G10B10A2_UNORM backbuffer pixel
// (PQ-encoded BT.2020 RGB, the HDR10 swap chain format) into 8-bit sRGB BGRA.
// Mirrors the capture shader's SDR-from-HDR pipeline so a saved frame matches
// the BT.2446A-derived luminance EETF displayed by the renderer. Per-pixel on
// the CPU, which is fine for a one-shot screenshot.
static void Hdr10PixelToBgra8(uint32_t px, uint8_t* outBgra)
{
    // SMPTE ST.2084 (PQ) inverse EOTF, one channel. Output 1.0 == 10000 nits.
    auto pqToLinear = [](float pq) -> float {
        const float m1 = 0.1593017578125f;
        const float m2 = 78.84375f;
        const float c1 = 0.8359375f;
        const float c2 = 18.8515625f;
        const float c3 = 18.6875f;
        float p   = std::pow(pq < 0.0f ? 0.0f : pq, 1.0f / m2);
        float num = p - c1; if (num < 0.0f) num = 0.0f;
        float den = c2 - c3 * p;
        return std::pow(num / den, 1.0f / m1);
    };
    auto compressToSdrGamut = [](float r, float g, float b, float mappedY) {
        float scale = 1.0f;
        const auto constrain = [&](float channel) {
            if (channel < 0.0f) return -mappedY / (channel - mappedY);
            if (channel > 1.0f) return (1.0f - mappedY) / (channel - mappedY);
            return 1.0f;
        };
        scale = std::min(scale, constrain(r));
        scale = std::min(scale, constrain(g));
        scale = std::min(scale, constrain(b));
        scale = std::max(0.0f, std::min(1.0f, scale));
        return std::array<float, 3>{
            std::max(0.0f, std::min(1.0f, mappedY + (r - mappedY) * scale)),
            std::max(0.0f, std::min(1.0f, mappedY + (g - mappedY) * scale)),
            std::max(0.0f, std::min(1.0f, mappedY + (b - mappedY) * scale)),
        };
    };
    // Linear to sRGB gamma (IEC 61966-2-1), one channel, clamped to [0, 1].
    auto linearToSrgb = [](float lin) -> float {
        if (lin < 0.0f) lin = 0.0f; else if (lin > 1.0f) lin = 1.0f;
        return (lin <= 0.0031308f) ? lin * 12.92f
                                   : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
    };

    // Unpack R10G10B10A2_UNORM: R bits 0..9, G bits 10..19, B bits 20..29.
    float pqR = ((px      ) & 0x3FF) / 1023.0f;
    float pqG = ((px >> 10) & 0x3FF) / 1023.0f;
    float pqB = ((px >> 20) & 0x3FF) / 1023.0f;

    float r = pqToLinear(pqR);
    float g = pqToLinear(pqG);
    float b = pqToLinear(pqB);

    // BT.2020 to BT.709 primaries in linear light (ITU-R BT.2087-0).
    float r709 =  1.6605f * r - 0.5876f * g - 0.0728f * b;
    float g709 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
    float b709 = -0.0182f * r - 0.1006f * g + 1.1187f * b;

    const float originalY = std::max(0.0f,
        0.2126f * r709 + 0.7152f * g709 + 0.0722f * b709);
    const float mappedY = static_cast<float>(
        HdrToneMap::Bt2446ADerivedLuminance(originalY * 10000.0f));
    const float scale = originalY > 1e-6f ? mappedY / originalY : 0.0f;
    const auto mapped = compressToSdrGamut(r709 * scale, g709 * scale,
                                            b709 * scale, mappedY);

    float sr = linearToSrgb(mapped[0]);
    float sg = linearToSrgb(mapped[1]);
    float sb = linearToSrgb(mapped[2]);

    int R8 = (int)(sr * 255.0f + 0.5f); if (R8 < 0) R8 = 0; if (R8 > 255) R8 = 255;
    int G8 = (int)(sg * 255.0f + 0.5f); if (G8 < 0) G8 = 0; if (G8 > 255) G8 = 255;
    int B8 = (int)(sb * 255.0f + 0.5f); if (B8 < 0) B8 = 0; if (B8 > 255) B8 = 255;

    // BGRA byte order, matching the BMP BITMAPV4HEADER masks and the SDR path.
    outBgra[0] = (uint8_t)B8;
    outBgra[1] = (uint8_t)G8;
    outBgra[2] = (uint8_t)R8;
    outBgra[3] = 0xFF;
}

// HDR10 screenshot, true-HDR path: convert one R10G10B10A2_UNORM PQ BT.2020
// backbuffer pixel into scRGB FP16 (linear, BT.709 primaries, 1.0 == 80 nits):
// the encoding Windows uses for HDR JPEG XR. Reuses the PQ inverse-EOTF and the
// BT.2020->BT.709 matrix from the SDR path but stops before the tonemap, so
// highlights stay above 1.0 and wide-gamut colors stay outside [0,1]; FP16
// carries both. Output order is R,G,B,A halves (GUID_WICPixelFormat64bppRGBAHalf).
static void Hdr10PixelToScrgbHalf(uint32_t px, uint16_t* outRgbaHalf)
{
    auto pqToLinear = [](float pq) -> float {
        const float m1 = 0.1593017578125f;
        const float m2 = 78.84375f;
        const float c1 = 0.8359375f;
        const float c2 = 18.8515625f;
        const float c3 = 18.6875f;
        float p   = std::pow(pq < 0.0f ? 0.0f : pq, 1.0f / m2);
        float num = p - c1; if (num < 0.0f) num = 0.0f;
        float den = c2 - c3 * p;
        return std::pow(num / den, 1.0f / m1);   // 1.0 == 10000 nits
    };

    // Unpack R10G10B10A2_UNORM: R bits 0..9, G bits 10..19, B bits 20..29.
    float pqR = ((px      ) & 0x3FF) / 1023.0f;
    float pqG = ((px >> 10) & 0x3FF) / 1023.0f;
    float pqB = ((px >> 20) & 0x3FF) / 1023.0f;

    float r = pqToLinear(pqR);
    float g = pqToLinear(pqG);
    float b = pqToLinear(pqB);

    // BT.2020 -> BT.709 primaries in linear light (ITU-R BT.2087-0).
    float r709 =  1.6605f * r - 0.5876f * g - 0.0728f * b;
    float g709 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
    float b709 = -0.0182f * r - 0.1006f * g + 1.1187f * b;

    // scRGB reference white is 80 nits; pqToLinear's 1.0 is 10000 nits.
    const float kToScrgb = 10000.0f / 80.0f;   // 125
    outRgbaHalf[0] = DirectX::PackedVector::XMConvertFloatToHalf(r709 * kToScrgb);
    outRgbaHalf[1] = DirectX::PackedVector::XMConvertFloatToHalf(g709 * kToScrgb);
    outRgbaHalf[2] = DirectX::PackedVector::XMConvertFloatToHalf(b709 * kToScrgb);
    outRgbaHalf[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
}

// Encode a top-down BGRA8 buffer to a PNG via Windows Imaging Component.
// PNG keeps the screenshot lossless but compressed and shareable anywhere
// (a raw BMP of a 4K frame is about 33 MB and many apps will not preview or
// accept it). COM is already initialized on the render thread by the app host.
static bool WritePngBgra8(const std::wstring& path, const uint8_t* pixels,
                          uint32_t width, uint32_t height, uint32_t stride)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    if (FAILED(frame->WritePixels(height, stride, stride * height,
                                  const_cast<BYTE*>(pixels)))) return false;

    if (FAILED(frame->Commit()))   return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

// Encode a top-down scRGB FP16 (64bpp RGBA half-float) buffer to a JPEG XR
// (.jxr) via Windows Imaging Component. scRGB (linear, BT.709 primaries, value
// 1.0 == 80 nits) in FP16 is the encoding Windows itself uses for HDR
// screenshots: the Photos app recognizes a 64bppRGBAHalf .jxr as HDR and
// tone-maps it back for SDR displays, so no embedded color profile is needed.
// Highlights above 80 nits sit above 1.0 and wide-gamut colors outside BT.709
// sit outside [0,1]; FP16 carries both, which integer formats cannot. The JXR
// encoder supports 64bppRGBAHalf natively.
static bool WriteJxrScrgbHalf(const std::wstring& path, const uint8_t* pixels,
                              uint32_t width, uint32_t height, uint32_t stride)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;

    // Max quality so the float HDR survives the encode.
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 1.0f;
        props->Write(1, &opt, &v);
        VariantClear(&v);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat64bppRGBAHalf;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    // The encoder must keep the float format; if it substituted an integer one
    // the HDR would be silently clamped, so skip the JXR rather than save SDR.
    if (fmt != GUID_WICPixelFormat64bppRGBAHalf) return false;

    if (FAILED(frame->WritePixels(height, stride, stride * height,
                                  const_cast<BYTE*>(pixels)))) return false;

    if (FAILED(frame->Commit()))   return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

bool DX11Renderer::SaveScreenshot(const std::wstring& path)
{
    if (!m_swapChain) {
        OutputDebugStringW(L"[NitLink/Renderer] Screenshot: no swap chain\n");
        return false;
    }

    // Read the swap chain backbuffer (post-shader, display-correct image)
    // instead of the raw planar capture texture. Must be called before
    // EndFrame()'s Present, since FLIP_DISCARD leaves buffer 0 undefined
    // after Present. See the dispatch call sites in Application::Run.
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        OutputDebugStringW(L"[NitLink/Renderer] Screenshot: GetBuffer failed\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    backBuffer->GetDesc(&srcDesc);

    // SDR uses a BGRA8 backbuffer and copies straight through. HDR mode
    // reconfigures the swap chain to R10G10B10A2_UNORM with BT.2020 PQ; that
    // case tonemaps every pixel to 8-bit sRGB during readback below (see
    // Hdr10PixelToBgra8). Any other format is unexpected, so bail.
    const bool srcIsHdr10 = (srcDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    if (srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && !srcIsHdr10) {
        OutputDebugStringW(L"[NitLink/Renderer] Screenshot: unsupported backbuffer format\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags      = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags      = 0;

    ComPtr<ID3D11Texture2D> stagingTex;
    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) return false;

    m_context->CopyResource(stagingTex.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint32_t width  = srcDesc.Width;
    const uint32_t height = srcDesc.Height;
    const uint32_t rowBytes = width * 4;

    std::vector<uint8_t> pixelData;
    try {
        pixelData.resize((size_t)width * height * 4);
    } catch (...) {
        m_context->Unmap(stagingTex.Get(), 0);
        return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = pixelData.data();
    const size_t   srcPitch = mapped.RowPitch;

    // True-HDR export buffer: scRGB FP16 (64bpp = 8 bytes/pixel vs the SDR PNG's
    // 4), built alongside the SDR tonemap below from the same source pixel and
    // encoded to a .jxr so the full HDR is preserved, not only the 8-bit view.
    std::vector<uint8_t> hdrData;
    const uint32_t hdrRowBytes = width * 8;   // 4 channels * 16-bit half
    if (srcIsHdr10) {
        try { hdrData.resize((size_t)hdrRowBytes * height); } catch (...) { hdrData.clear(); }
        // HDR10 backbuffer: tonemap each PQ BT.2020 pixel to 8-bit sRGB BGRA for
        // the shareable SDR PNG, AND convert the same pixel to scRGB FP16 for the
        // true-HDR .jxr (no tonemap; highlights and wide gamut preserved).
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* sRow = src + y * srcPitch;
            uint8_t*       dRow = dst + y * rowBytes;
            uint16_t*      hRow = hdrData.empty() ? nullptr
                : reinterpret_cast<uint16_t*>(hdrData.data() + (size_t)y * hdrRowBytes);
            for (uint32_t x = 0; x < width; x++) {
                uint32_t px;
                memcpy(&px, sRow + (size_t)x * 4, sizeof(px));
                Hdr10PixelToBgra8(px, dRow + (size_t)x * 4);
                if (hRow) Hdr10PixelToScrgbHalf(px, hRow + (size_t)x * 4);
            }
        }
    } else {
        // SDR backbuffer is already full-range display sRGB BGRA8. Direct copy.
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* sRow = src + y * srcPitch;
            uint8_t*       dRow = dst + y * rowBytes;
            memcpy(dRow, sRow, rowBytes);
        }
    }

    m_context->Unmap(stagingTex.Get(), 0);
    stagingTex.Reset();

    // Save as PNG: lossless but compressed and shareable anywhere (a raw BMP
    // of a 4K frame is about 33 MB and chat clients will not preview or accept
    // it). pixelData is top-down BGRA8, the natural row order WIC expects.
    std::wstring pngPath = path;
    size_t dot = pngPath.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        pngPath.replace(dot, std::wstring::npos, L".png");
    } else {
        pngPath += L".png";
    }

    bool ok = WritePngBgra8(pngPath, pixelData.data(), width, height, rowBytes);

    if (ok) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] Screenshot saved: " << pngPath << L"\n";
        OutputDebugStringW(ss.str().c_str());
    } else {
        OutputDebugStringW(L"[NitLink/Renderer] Screenshot: PNG encode failed\n");
    }

    // True-HDR sidecar: in HDR mode also write the scRGB FP16 buffer to a .jxr
    // (JPEG XR), the format Windows uses for HDR screenshots, so Photos opens
    // it as real HDR. The SDR PNG above is the shareable tonemapped view.
    if (srcIsHdr10 && !hdrData.empty()) {
        std::wstring jxrPath = path;
        size_t jdot = jxrPath.find_last_of(L'.');
        if (jdot != std::wstring::npos) jxrPath.replace(jdot, std::wstring::npos, L".jxr");
        else                            jxrPath += L".jxr";
        if (WriteJxrScrgbHalf(jxrPath, hdrData.data(), width, height, hdrRowBytes)) {
            std::wstringstream ss;
            ss << L"[NitLink/Renderer] HDR screenshot saved: " << jxrPath << L"\n";
            OutputDebugStringW(ss.str().c_str());
        } else {
            OutputDebugStringW(L"[NitLink/Renderer] Screenshot: JXR encode failed\n");
        }
    }
    return ok;
}

// ============================================================================
// HDR support
// ============================================================================
//
// HDR output uses the R10G10B10A2_UNORM swap chain with
// DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10: ST.2084 PQ + BT.2020
// primaries). This is the encoding HDR10 displays consume natively:
// values flow through NitLink without an app-side scRGB intermediate or
// SDR tonemap.
//
// This path differs from the FP16-scRGB approach Windows samples often
// show; rationale for the choice is detailed at SetHDREnabled() below.
//
// References:
//   https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
//   https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12HDR

void DX11Renderer::QueryHDRDisplayInfo()
{
    // Walk DXGI: device -> adapter -> the output whose desktop area contains
    // the main window, then query IDXGIOutput6 for HDR capability and luminance.
    // If anything in this chain fails, conservatively report SDR-only.
    m_hdrDisplaySupported = false;
    m_hdrEngagedByOS      = false;
    m_displayMinLum       = 0.0f;
    m_displayMaxLum       = 80.0f;

    if (!m_device || !m_swapChain) return;

    // CRITICAL: create a FRESH IDXGIFactory1 every call instead of walking
    // the device's cached adapter. DXGI caches output descriptors against
    // the factory chain at creation time, so when the user toggles Windows
    // HDR on or off, the existing factory (and any outputs enumerated from
    // it) keeps returning the OLD ColorSpace value until the factory is
    // recreated. The device-derived adapter used previously WAS such a
    // stale enumeration: m_hdrEngagedByOS never flipped to true even
    // after the user engaged Windows HDR, so Alt+H kept refusing.
    //
    // Per Microsoft DXGI docs: "If you change the desktop display
    // configuration, you must release the DXGI factory and create a new
    // one. Calling existing IDXGIOutput methods returns cached values."
    //
    // A fresh CreateDXGIFactory1 every call is cheap (microseconds): this
    // function only runs on HDR-toggle attempts and on startup, not in the
    // render hot path.
    ComPtr<IDXGIFactory1> freshFactory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&freshFactory)))) return;

    // Find the output whose monitor rect contains the window. Walk all
    // adapters' outputs (cheap; typically 1-2 adapters with 1-4 outputs
    // total) because the fresh factory has no notion of which adapter the
    // device sits on, and the window's monitor uniquely identifies the
    // right output anyway.
    RECT windowRect{};
    GetWindowRect(m_hwnd, &windowRect);
    const int wx = (windowRect.left + windowRect.right) / 2;
    const int wy = (windowRect.top  + windowRect.bottom) / 2;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIdx = 0;
         freshFactory->EnumAdapters1(adapterIdx, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++adapterIdx)
    {
    UINT outputIdx = 0;
    ComPtr<IDXGIOutput> output;
    while (adapter->EnumOutputs(outputIdx++, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) { output.Reset(); continue; }
        if (wx >= desc.DesktopCoordinates.left && wx < desc.DesktopCoordinates.right &&
            wy >= desc.DesktopCoordinates.top  && wy < desc.DesktopCoordinates.bottom)
        {
            ComPtr<IDXGIOutput6> out6;
            if (SUCCEEDED(output.As(&out6))) {
                DXGI_OUTPUT_DESC1 d1{};
                if (SUCCEEDED(out6->GetDesc1(&d1))) {
                    // d1.ColorSpace indicates what Windows is currently sending
                    // to this display. If it's HDR10/PQ then Windows HDR mode
                    // is engaged for this output.
                    m_hdrEngagedByOS = (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    // Treat any display reporting HDR colorspace as HDR-
                    // capable. Some monitors report HDR support even when
                    // Windows HDR mode is off, via EDID; both are captured
                    // here via the engaged flag and a more liberal "supported"
                    // check below.
                    m_hdrDisplaySupported = m_hdrEngagedByOS
                                          || (d1.MaxLuminance > 100.0f);
                    m_displayMinLum = d1.MinLuminance;
                    m_displayMaxLum = d1.MaxLuminance > 0.0f ? d1.MaxLuminance : 80.0f;

                    // Only log when something materially changed. Without
                    // this gate the per-frame status callback floods the
                    // Output window and drowns out other diagnostics
                    // (toggle clicks, SetHDREnabled outcomes, etc).
                    static DXGI_COLOR_SPACE_TYPE lastColorSpace = (DXGI_COLOR_SPACE_TYPE)-1;
                    static float lastMaxLum = -1.0f;
                    if (d1.ColorSpace != lastColorSpace ||
                        d1.MaxLuminance != lastMaxLum)
                    {
                        std::wstringstream ss;
                        ss << L"[NitLink/Renderer] Display HDR info: "
                           << L"colorSpace=" << d1.ColorSpace
                           << L" engaged=" << (m_hdrEngagedByOS ? L"yes" : L"no")
                           << L" supported=" << (m_hdrDisplaySupported ? L"yes" : L"no")
                           << L" min=" << d1.MinLuminance
                           << L" max=" << d1.MaxLuminance
                           << L" maxFull=" << d1.MaxFullFrameLuminance
                           << L"\n";
                        OutputDebugStringW(ss.str().c_str());
                        lastColorSpace = d1.ColorSpace;
                        lastMaxLum = d1.MaxLuminance;
                    }
                }
            }
            // Matched output: stop scanning. Reset both iterators so the
            // outer adapter loop sees a clean state on its break check.
            output.Reset();
            adapter.Reset();
            return;
        }
        output.Reset();
    }
        adapter.Reset();
    } // for adapter
}

bool DX11Renderer::CreateHDRTestShader()
{
    if (m_hdrTestPS) return true;

    // HDR test pattern: a grid of 12 squares with increasing scRGB luminance,
    // labeled internally as 80/100/150/200/300/500/750/1000/1500/2000/3000/4000
    // nits. On an HDR display this produces a brightness ladder where the
    // higher squares should look visibly brighter than the SDR-white square.
    // On an SDR display all the >1.0 values will clip to white.
    //
    // The shader also paints a colored background (subtle dark blue gradient)
    // at SDR brightness so the contrast against the highlight squares is obvious.
    //
    // scRGB scale: 1.0 = 80 nits, so target_nits / 80 = scRGB value.
    const char* psSrc = R"(
struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PS_IN i) : SV_TARGET {
    // Dark gradient background, well within SDR
    float3 bg = float3(0.02, 0.03, 0.06) + i.uv.y * float3(0.05, 0.05, 0.10);

    // 4x3 grid of brightness squares centered horizontally, occupying the
    // top 60% of the screen. Each square is a uniform scRGB color.
    const float gridY0 = 0.10;
    const float gridY1 = 0.70;
    const float gridX0 = 0.10;
    const float gridX1 = 0.90;

    float3 col = bg;

    if (i.uv.x >= gridX0 && i.uv.x <= gridX1 &&
        i.uv.y >= gridY0 && i.uv.y <= gridY1)
    {
        float gx = (i.uv.x - gridX0) / (gridX1 - gridX0); // 0..1
        float gy = (i.uv.y - gridY0) / (gridY1 - gridY0);
        int cx = (int)floor(gx * 4.0);  // 0..3
        int cy = (int)floor(gy * 3.0);  // 0..2
        int idx = cy * 4 + cx;

        // scRGB values: target_nits / 80
        float ladder[12] = {
             1.000,  1.250,  1.875,  2.500,   //  80, 100, 150, 200
             3.750,  6.250,  9.375, 12.500,   // 300, 500, 750, 1000
            18.750, 25.000, 37.500, 50.000    // 1500, 2000, 3000, 4000
        };

        // Add tiny inner-border so cells are visually separated
        float cellGx = frac(gx * 4.0);
        float cellGy = frac(gy * 3.0);
        if (cellGx > 0.05 && cellGx < 0.95 && cellGy > 0.05 && cellGy < 0.95) {
            float v = ladder[idx];
            col = float3(v, v, v);
        }
    }

    // Small colored swatches along the bottom 20% to verify wide gamut /
    // saturated channels: pure red/green/blue at HDR brightness (500 nits).
    const float swY0 = 0.78;
    const float swY1 = 0.92;
    if (i.uv.y >= swY0 && i.uv.y <= swY1) {
        if      (i.uv.x > 0.10 && i.uv.x < 0.30) col = float3(6.25, 0.0,  0.0);
        else if (i.uv.x > 0.40 && i.uv.x < 0.60) col = float3(0.0,  6.25, 0.0);
        else if (i.uv.x > 0.70 && i.uv.x < 0.90) col = float3(0.0,  0.0,  6.25);
    }

    return float4(col, 1.0);
}
)";

    ComPtr<ID3DBlob> psBlob, err;
    HRESULT hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
                              "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                              0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        return false;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                       nullptr, &m_hdrTestPS);
    return SUCCEEDED(hr);
}

bool DX11Renderer::RecreateSwapChainBuffer(DXGI_FORMAT newFormat)
{
    if (!m_swapChain) return false;
    if (newFormat == m_currentSwapFormat) return true;

    // Release the backbuffer RTV before resize. Caller is responsible for
    // releasing D2D bitmaps wrapping the backbuffer (overlay does this via
    // OnResizeBegin/End: same flow used for window resize).
    m_rtv.Reset();
    m_context->Flush();

    // ResizeBuffers with the new format. Same backbuffer count and flags.
    // Flags MUST match the original creation flags (incl. waitable object),
    // otherwise DXGI returns an error.
    HRESULT hr = m_swapChain->ResizeBuffers(
        0, m_windowWidth, m_windowHeight,
        newFormat,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"[NitLink/Renderer] SetHDR ResizeBuffers failed hr=0x"
           << std::hex << hr << L"\n";
        OutputDebugStringW(ss.str().c_str());
        // A device removed during the HDR format swap surfaces here rather
        // than at Present. Route it into the same latch the run loop polls so
        // recovery engages instead of leaving the swap chain unrecreated.
        FlagIfDeviceLost(hr, L"RecreateSwapChainBuffer ResizeBuffers");
        return false;
    }

    m_currentSwapFormat = newFormat;
    if (!CreateRenderTarget()) return false;

    std::wstringstream ss;
    ss << L"[NitLink/Renderer] swap chain backbuffer recreated as format="
       << newFormat << L"\n";
    OutputDebugStringW(ss.str().c_str());
    return true;
}

bool DX11Renderer::SetHDREnabled(bool enable)
{
    if (enable == m_hdrEnabled) return true;

    // Refresh the display's HDR state first so the right status is reported
    // back to settings UI even if HDR engagement fails.
    QueryHDRDisplayInfo();

    if (enable) {
        // Gate: HDR10 requires Windows HDR to be ENGAGED at the OS level.
        //
        // The R10G10B10A2 + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 swap
        // chain only delivers correct PQ BT.2020 to the panel when Windows
        // is in HDR mode system-wide (Settings -> Display -> HDR toggle on,
        // or Win+Alt+B). With Windows HDR off, the swap chain creation
        // succeeds and SetColorSpace1 returns S_OK, but DWM composites
        // through an SDR fallback path that interprets the PQ-encoded BT.2020
        // codes as something other than PQ: channels get mangled and the
        // image is nonsense. (Microsoft docs are explicit: R10G10B10A2 +
        // G2084_P2020 is HDR-only.)
        //
        // QueryHDRDisplayInfo populates m_hdrEngagedByOS from the output
        // descriptor's ColorSpace (==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        // when system HDR is engaged for this monitor).
        if (!m_hdrEngagedByOS) {
            OutputDebugStringW(L"[NitLink/Renderer] HDR: Windows HDR not engaged at OS level, refusing to enable HDR mode\n");
            OutputDebugStringW(L"[NitLink/Renderer] HDR: enable HDR in Windows Display Settings (Win+Alt+B) then try again\n");
            return false;
        }

        // The HDR test shader is diagnostic-only (consumed by DrawHDRTestPattern,
        // which has no production caller). A compile failure must not block
        // real HDR mode. DrawHDRTestPattern null-guards m_hdrTestPS, so if
        // CreateHDRTestShader fails the diagnostic path is simply unavailable
        // for the rest of this session.
        if (!CreateHDRTestShader()) {
            OutputDebugStringW(L"[NitLink/Renderer] HDR test shader compile failed; diagnostic test pattern unavailable, real HDR path continues\n");
        }

        ComPtr<IDXGISwapChain3> sc3;
        if (FAILED(m_swapChain.As(&sc3))) {
            OutputDebugStringW(L"[NitLink/Renderer] HDR: IDXGISwapChain3 not available\n");
            return false;
        }

        // IMPORTANT: CheckColorSpaceSupport is sensitive to the swap chain's
        // CURRENT backbuffer format, not a hypothetical target format. Asking
        // "does this BGRA chain support HDR10?" correctly returns NO. The
        // chain must be recreated as R10G10B10A2 first, THEN checked, THEN
        // the colorspace set.
        //
        // Why R10G10B10A2 + G2084_NONE_P2020 instead of FP16 + scRGB?
        //
        // FP16 scRGB is the "video editor" HDR path: linear-light values
        // in BT.709 primaries, with negative values used to represent
        // wide-gamut colors. The OS composites and converts to display.
        // This is FLEXIBLE but adds two conversions to the pipeline (the
        // P010 source has to be PQ-decoded to linear, then DWM has to
        // PQ-encode to send to the display).
        //
        // R10G10B10A2 + G2084_NONE_P2020 is the "video player" HDR path:
        // PQ-encoded values in BT.2020 primaries, presented directly to
        // the display without conversion. This is EXACTLY what HDR10
        // content (P010, HEVC HDR, MKV HDR) wants. Zero color-space
        // conversion in the entire pipeline.
        //
        // Capturing HDR10 from a PS5 and displaying it on an HDR10 OLED,
        // this is the natural format. Verified working in MPV/madVR for
        // HDR10 video playback. The P010 shader outputs PQ-encoded BT.2020
        // directly into this swap chain: same flow as a video player
        // presenting HDR10 content.
        if (!RecreateSwapChainBuffer(DXGI_FORMAT_R10G10B10A2_UNORM)) return false;

        // HDR10: ST.2084 PQ + BT.2020 primaries. This is the colorspace
        // every HDR10 display understands natively (in fact it's typically
        // the ONLY HDR colorspace the wire transmits to the panel).
        const DXGI_COLOR_SPACE_TYPE targetCS =
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

        UINT csSupport = 0;
        sc3->CheckColorSpaceSupport(targetCS, &csSupport);
        if (!(csSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
            OutputDebugStringW(L"[NitLink/Renderer] HDR: HDR10 colorspace not supported on R10G10B10A2 chain, falling back to SDR\n");
            RecreateSwapChainBuffer(DXGI_FORMAT_B8G8R8A8_UNORM);
            return false;
        }

        HRESULT hr = sc3->SetColorSpace1(targetCS);
        if (FAILED(hr)) {
            std::wstringstream ss;
            ss << L"[NitLink/Renderer] HDR: SetColorSpace1 failed hr=0x"
               << std::hex << hr << L"\n";
            OutputDebugStringW(ss.str().c_str());
            RecreateSwapChainBuffer(DXGI_FORMAT_B8G8R8A8_UNORM);
            return false;
        }

        OutputDebugStringW(L"[NitLink/Renderer] HDR colorspace: HDR10 PQ BT.2020 (native)\n");

        // Send HDR10 static metadata (SMPTE ST.2086 mastering display +
        // CEA-861.3 MaxCLL/MaxFALL). This tells the display:
        //
        //   "This is professional HDR10 content mastered to a 1000-nit
        //    BT.2020 display, with peak brightness ~1000 nits and
        //    average frame brightness ~400 nits."
        //
        // Without this metadata, displays often play it safe: they assume
        // the worst case (10000-nit highlights possible) and tonemap very
        // conservatively, lifting shadows to avoid black crush. With proper
        // metadata, the display knows the content's actual range and can
        // preserve deeper shadows (and on the LG C3 specifically, may pick
        // its "HDR Game" picture mode automatically rather than the more
        // conservative "HDR Standard").
        //
        // Values chosen to match the standard HDR10 game-mastering target
        // that the PS5 itself signals when sending HDR10 content without
        // per-title metadata. This is also the de-facto convention for
        // every modern HDR game (Sony first-party titles, Insomniac,
        // Naughty Dog, Capcom RE engine, FromSoft etc.).
        //
        // CAVEAT: per Microsoft's docs, "Applications should not rely on
        // the metadata being sent to the monitor as the metadata may be
        // ignored. Monitors do not consistently process HDR metadata."
        // So this is best-effort: if the C3 ignores it, nothing is lost.
        // If it honors it, shadows snap deeper.
        //
        // Static defaults match what most HDR games signal. Per-source
        // mastering metadata is available from the Elgato HDR InfoFrame
        // packet (bytes 5..31) if finer fidelity is wanted; the static
        // values cover the common case.
        ComPtr<IDXGISwapChain4> sc4;
        if (SUCCEEDED(m_swapChain.As(&sc4))) {
            DXGI_HDR_METADATA_HDR10 meta{};
            // BT.2020 primaries (the wide-gamut HDR10 standard).
            // Per DXGI spec, primary chromaticities are encoded as
            // (chroma x 50000) so 0.708 R-x becomes 35400.
            meta.RedPrimary[0]   = static_cast<UINT16>(0.708 * 50000);
            meta.RedPrimary[1]   = static_cast<UINT16>(0.292 * 50000);
            meta.GreenPrimary[0] = static_cast<UINT16>(0.170 * 50000);
            meta.GreenPrimary[1] = static_cast<UINT16>(0.797 * 50000);
            meta.BluePrimary[0]  = static_cast<UINT16>(0.131 * 50000);
            meta.BluePrimary[1]  = static_cast<UINT16>(0.046 * 50000);
            // D65 white point.
            meta.WhitePoint[0]   = static_cast<UINT16>(0.3127 * 50000);
            meta.WhitePoint[1]   = static_cast<UINT16>(0.3290 * 50000);
            // Mastering display luminance. Max is in nits as UINT, min is
            // encoded as (nits x 10000) per DXGI convention.
            meta.MaxMasteringLuminance = 1000;
            meta.MinMasteringLuminance = static_cast<UINT>(0.005 * 10000);
            // Content light levels. Standard HDR10 game mastering.
            meta.MaxContentLightLevel        = 1000;  // peak pixel
            meta.MaxFrameAverageLightLevel   = 400;   // avg frame

            HRESULT mhr = sc4->SetHDRMetaData(
                DXGI_HDR_METADATA_TYPE_HDR10,
                sizeof(meta),
                &meta);
            if (SUCCEEDED(mhr)) {
                OutputDebugStringW(L"[NitLink/Renderer] HDR10 metadata sent: BT.2020 / 1000nit / MaxCLL=1000 / MaxFALL=400\n");
            } else {
                std::wstringstream ss;
                ss << L"[NitLink/Renderer] HDR10 metadata SetHDRMetaData failed hr=0x"
                   << std::hex << mhr << L" (display may apply conservative tonemap)\n";
                OutputDebugStringW(ss.str().c_str());
            }
        } else {
            OutputDebugStringW(L"[NitLink/Renderer] IDXGISwapChain4 unavailable, skipping HDR10 metadata\n");
        }

        // The P010 shader now outputs PQ-encoded BT.2020 directly to this
        // chain: zero conversion needed. The BT.2020-to-BT.709 matrix flag
        // is no longer relevant (primaries no longer need rotation).
        m_hdrUseBT709Matrix = false;

        m_hdrEnabled = true;
        OutputDebugStringW(L"[NitLink/Renderer] HDR mode ENABLED (R10G10B10A2 + HDR10 PQ)\n");
        return true;
    } else {
        // Back to SDR. Reset colorspace first (BGRA's implicit colorspace is
        // sRGB-G22-P709, which is what SetColorSpace1 reports as 0).
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(m_swapChain.As(&sc3))) {
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
        if (!RecreateSwapChainBuffer(DXGI_FORMAT_B8G8R8A8_UNORM)) return false;
        m_hdrEnabled = false;
        m_hdrUseBT709Matrix = false;
        OutputDebugStringW(L"[NitLink/Renderer] HDR mode DISABLED (back to SDR)\n");
        return true;
    }
}

void DX11Renderer::DrawHDRTestPattern()
{
    if (!m_hdrTestPS || !m_rtv) return;

    // Bind the backbuffer as RTV and run a fullscreen pass with the test
    // shader. Reuses the existing fullscreen quad / vertex shader / sampler.
    D3D11_VIEWPORT vp{ 0, 0, (float)m_windowWidth, (float)m_windowHeight, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    // Ensure the vertex shader's transform is identity (full-screen quad).
    // The normal capture path updates this CB, but HDR test pattern runs
    // standalone, so set it explicitly here or the quad collapses to 0.
    struct { float sx, sy, px, py; } identity = { 1.0f, 1.0f, 0.0f, 0.0f };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &identity, sizeof(identity));
        m_context->Unmap(m_transformCB.Get(), 0);
    }
    m_context->VSSetConstantBuffers(0, 1, m_transformCB.GetAddressOf());

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_hdrTestPS.Get(), nullptr, 0);

    UINT stride = sizeof(float) * 4, offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->Draw(4, 0);
}

void DX11Renderer::DrawHDRDiagnostics()
{
    if (!m_rtv) return;

    // Lazily compile the diagnostic shader on first use. Same fullscreen
    // quad as DrawHDRTestPattern, but the shader paints calibrated
    // reference patches over the bottom band of the screen and leaves
    // the top portion fully transparent (alpha=0) so the captured frame
    // shows through. The patches start with known linear BT.709 values
    // in scRGB units, then convert to BT.2020 and PQ for the HDR10 target.
    // This permits visual A/B
    // against the same patches rendered natively (PS5 direct to TV via
    // the diag test pattern).
    if (!m_hdrDiagPS) {
        const char* psSrc = R"(
struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PS_IN i) : SV_TARGET {
    // Top 75% of screen: transparent so capture shows through.
    if (i.uv.y < 0.75) {
        return float4(0, 0, 0, 0);
    }

    // Bottom 25%: 6 vertical reference patches.
    // scRGB units: 1.0 = 80 nits (SDR reference white).
    //
    // PATCH LAYOUT (left to right):
    //   1. 100% white at SDR ref (203 nits target: Windows HDR's default
    //      SDR-content brightness). If this looks WARM in NitLink's pipeline
    //      vs direct-to-TV, there's a global color-temperature bias.
    //   2. Mid-grey (50% scRGB).
    //   3. Pure reference orange: sRGB(255, 128, 0) at SDR brightness.
    //      This is THE diagnostic patch. If it shifts, the exact amount
    //      the pipeline is reddening oranges is measurable.
    //   4. Pure red (sRGB(255, 0, 0)) at SDR brightness.
    //   5. Reference skin tone: sRGB(224, 172, 145), neutral caucasian.
    //      Human eyes catch skin-tone drift instantly.
    //   6. HDR bright white (1000 nits): sanity check that the HDR
    //      path is actually pushing values past SDR clip.
    float patch = floor((i.uv.x) * 6.0);
    float3 col;

    // SDR reference scale: target brightness 203 nits / 80 nits-per-scRGB = 2.5375
    const float sdrRef = 203.0 / 80.0;

    if (patch < 1.0) {
        // 1. Pure white at SDR ref
        col = float3(sdrRef, sdrRef, sdrRef);
    } else if (patch < 2.0) {
        // 2. Mid-grey
        col = float3(sdrRef * 0.5, sdrRef * 0.5, sdrRef * 0.5);
    } else if (patch < 3.0) {
        // 3. Reference orange: sRGB(255, 128, 0) normalized = (1, 0.5, 0)
        //    Convert sRGB -> linear before scaling to scRGB ref.
        //    sRGB to linear approx: pow(c, 2.2)
        float3 srgb = float3(1.0, 0.502, 0.0);
        float3 lin  = pow(srgb, 2.2);
        col = lin * sdrRef;
    } else if (patch < 4.0) {
        // 4. Pure red
        col = float3(sdrRef, 0.0, 0.0);
    } else if (patch < 5.0) {
        // 5. Skin tone: sRGB(224, 172, 145)
        float3 srgb = float3(0.878, 0.675, 0.569);
        float3 lin  = pow(srgb, 2.2);
        col = lin * sdrRef;
    } else {
        // 6. HDR bright white at 1000 nits = 12.5 in scRGB
        col = float3(12.5, 12.5, 12.5);
    }

    // Small inner border so adjacent patches are visually separated.
    float patchU = frac(i.uv.x * 6.0);
    float patchV = (i.uv.y - 0.75) / 0.25; // 0..1 within the band
    if (patchU < 0.02 || patchU > 0.98 || patchV < 0.05 || patchV > 0.95) {
        col = float3(0, 0, 0);
    }

    float3 linear2020;
    linear2020.r = 0.6274 * col.r + 0.3293 * col.g + 0.0433 * col.b;
    linear2020.g = 0.0691 * col.r + 0.9195 * col.g + 0.0114 * col.b;
    linear2020.b = 0.0164 * col.r + 0.0880 * col.g + 0.8956 * col.b;
    // scRGB 1.0 = 80 nits. ST.2084 normalizes 10000 nits to 1.0.
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float3 lm1 = pow(max(linear2020 * (80.0 / 10000.0), 0.0), m1);
    float3 pq = pow((c1 + c2 * lm1) / (1.0 + c3 * lm1), m2);
    return float4(saturate(pq), 1.0);
}
)";
        ComPtr<ID3DBlob> psBlob, err;
        HRESULT hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
                                  "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &psBlob, &err);
        if (FAILED(hr)) {
            if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
            return;
        }
        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                           nullptr, &m_hdrDiagPS);
        if (FAILED(hr)) return;
    }

    // Enable alpha blending so the transparent top 75% lets the captured
    // frame show through. A one-shot blend state is required for this: the
    // normal capture path uses opaque rendering, so the default state has
    // blending disabled.
    if (!m_diagBlendState) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bd, &m_diagBlendState);
    }
    if (m_diagBlendState) {
        float bf[4] = { 1, 1, 1, 1 };
        m_context->OMSetBlendState(m_diagBlendState.Get(), bf, 0xFFFFFFFF);
    }

    D3D11_VIEWPORT vp{ 0, 0, (float)m_windowWidth, (float)m_windowHeight, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    // Identity transform for fullscreen quad.
    struct { float sx, sy, px, py; } identity = { 1.0f, 1.0f, 0.0f, 0.0f };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_context->Map(m_transformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &identity, sizeof(identity));
        m_context->Unmap(m_transformCB.Get(), 0);
    }
    m_context->VSSetConstantBuffers(0, 1, m_transformCB.GetAddressOf());

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_hdrDiagPS.Get(), nullptr, 0);

    UINT stride = sizeof(float) * 4, offset = 0;
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->Draw(4, 0);

    // Restore default (no-blend) state so subsequent passes aren't affected.
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

} // namespace NitLink
