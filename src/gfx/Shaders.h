#pragma once

// HLSL compiled at runtime with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
// Effects write linear color. The grade pass encodes sRGB, dithers, and applies the shared look.
static const char kSpriteHLSL[] = R"(
cbuffer Constants : register(b0) {
    float2 screenSize;
    float2 pad;
};

struct VSIn {
    uint vid : SV_VertexID;
    float4 rect : IRECT;
    float4 uv   : IUV;
    float4 col  : ICOL;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn i) {
    float2 corners[4] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0)
    };
    float2 c = corners[i.vid];
    float2 px = i.rect.xy + c * i.rect.zw;
    float2 ndc = float2(px.x / screenSize.x * 2.0 - 1.0,
                        1.0 - px.y / screenSize.y * 2.0);
    VSOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv  = lerp(i.uv.xy, i.uv.zw, c);
    o.col = i.col;
    return o;
}

Texture2D glyphTex : register(t0);
SamplerState glyphSamp : register(s0);

float3 toLin(float3 c) { return pow(saturate(c), 2.2); }

float4 PSMain(VSOut i) : SV_Target {
    float a = glyphTex.Sample(glyphSamp, i.uv).r * saturate(i.col.a);
    return float4(toLin(i.col.rgb) * a, a);
}
)";

static const char kFullscreenHLSL[] = R"(
cbuffer Constants : register(b0) {
    float time;
    float width;
    float height;
    float speed;
    float4 colorA;
    float4 colorB;
    float density;
    float p0;
    float p1;
    float p2;
    float direction;
    float lookX;
    float lookY;
    float p3;
    float scroll;
    float quality;
    float padA;
    float padB;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

Texture2D<float> noiseTex : register(t1);
SamplerState noiseSamp : register(s1);
// Photoreal plates. padA bits: 1 sky, 2 mountains, 4 mid, 8 foreground.
// Zero keeps the procedural stand-in. The farthest layer drifts the least.
Texture2D plateSky : register(t2);
Texture2D plateMtn : register(t3);
Texture2D plateMid : register(t4);
Texture2D plateFg : register(t5);

float2 plateShift(int which) {
    float depth = (which == 0) ? 0.22 : (which == 1) ? 0.55 : (which == 2) ? 1.05 : 1.75;
    float2 look = float2(lookX, -lookY) * (0.035 + 0.085 * depth);
    float ang = frac(time / 3600.0) * 6.2831853;
    float drift = sin(ang * 2.0 + float(which) * 0.7);
    float driftY = sin(ang + 1.4) * 0.35;
    float amp = 0.0012 + 0.0055 * depth;
    return look + float2(drift, driftY) * amp;
}

float4 plateLayer(int which, float2 uv) {
    float2 u = saturate(uv + plateShift(which));
    if (which <= 0) return plateSky.Sample(noiseSamp, u);
    if (which == 1) return plateMtn.Sample(noiseSamp, u);
    if (which == 2) return plateMid.Sample(noiseSamp, u);
    return plateFg.Sample(noiseSamp, u);
}

int plateBit(int layer) {
    if (layer <= 0) return 1;
    if (layer == 1) return 2;
    if (layer == 2) return 4;
    return 8;
}

void overPlate(inout float3 col, int layer, float2 uv) {
    int m = (int)(padA + 0.5);
    if ((m & plateBit(layer)) == 0) return;
    float4 s = plateLayer(layer, uv);
    col = lerp(col, s.rgb, s.a);
}

// Bright ice on the aurora plates picks up a little of the curtain behind it.
float3 coverPlates(float3 sky, float2 uv, float spill) {
    float3 col = sky;
    [unroll] for (int layer = 1; layer <= 3; layer++) {
        int m = (int)(padA + 0.5);
        if ((m & plateBit(layer)) == 0) continue;
        float4 s = plateLayer(layer, uv);
        float3 rgb = s.rgb;
        if (spill > 0.5) {
            float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            float ice = smoothstep(0.16, 0.55, luma);
            float g = max(sky.r, max(sky.g, sky.b));
            float3 tint = sky / max(g, 1e-3);
            // A hint of the curtain on the brightest snow. The plate itself stays near neutral.
            float catchLight = ice * saturate((g - 0.18) * 1.1) * 0.28;
            rgb = lerp(rgb, rgb * lerp(float3(1.0, 1.0, 1.0), tint, 0.40) + tint * 0.012, catchLight);
        }
        col = lerp(col, rgb, s.a);
    }
    return col;
}

float3 plateStack(float2 uv) {
    float3 c = float3(0.004, 0.006, 0.014);
    if (((int)(padA + 0.5) & 1) != 0) c = plateLayer(0, uv).rgb;
    overPlate(c, 1, uv);
    overPlate(c, 2, uv);
    overPlate(c, 3, uv);
    return c;
}

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p) {
    return float2(hash12(p), hash12(p + float2(19.1, 7.7)));
}

// One bilinear fetch. The UV is offset by the Hermite of the cell fraction,
// so hardware filtering is the value-noise interpolation.
float vnoise(float2 p) {
    float2 f = frac(p);
    float2 h = f * f * (3.0 - 2.0 * f);
    float2 uv = (floor(p) + h + 0.5) * (1.0 / 256.0);
    return noiseTex.SampleLevel(noiseSamp, uv, 0);
}

float vnoise1(float x) { return vnoise(float2(x, 0.37)); }

float fbm2(float2 p) {
    float s = 0.5 * vnoise(p);
    s += 0.25 * vnoise(p * 2.03 + float2(1.7, 9.2));
    return s;
}

float fbm3(float2 p) {
    float s = 0.5 * vnoise(p);
    p = p * 2.03 + float2(1.7, 9.2);
    s += 0.25 * vnoise(p);
    p = p * 2.03 + float2(1.7, 9.2);
    s += 0.125 * vnoise(p);
    return s;
}

float ridged(float x, float freq, float seed) {
    float n = vnoise(float2(x * freq, seed));
    float r = 1.0 - abs(n * 2.0 - 1.0);
    return r * r;
}

float3 starColor(float u) {
    float3 warm = float3(1.0, 0.58, 0.32);
    float3 white = float3(0.94, 0.96, 1.0);
    float3 blue = float3(0.62, 0.76, 1.0);
    float3 c = lerp(warm, white, smoothstep(0.0, 0.42, u));
    return lerp(c, blue, smoothstep(0.42, 1.0, u));
}

// Hue-preserving shoulder. The old per-channel 1-exp curve pulled bright
// greens and oranges toward white. Map the peak, keep the ratios.
float3 tonemap(float3 c) {
    c = max(c, 0.0);
    float m = max(c.r, max(c.g, c.b));
    float t = 1.0 - exp(-1.1 * m);
    return c * (t / max(m, 1e-4));
}
float3 toLin(float3 c) { return pow(saturate(c), 2.2); }

// field > 0.5 is the night-sky population: a shallower magnitude curve and a core
// wide enough to land on a pixel at 1200p, so the point survives fp16 + grade.
// field == 0 keeps the small aurora pinpricks.
void starPoint(inout float3 col, float2 cell, float2 f, float scale, float depth, float keep,
               float twinkle, float density, float3 tint, float pxPer, float field) {
    float n = hash12(cell + float2(3.1, depth * 9.0));
    float gate = 1.0 - keep;
    if (n <= gate) return;
    float t = saturate((n - gate) / max(keep, 0.001));
    bool sky = field > 0.5;
    float bright = pow(t, sky ? 1.7 : 6.0);
    float2 j = hash22(cell + float2(depth * 17.0, 4.0));
    float2 d = f - (j - 0.5) * (sky ? 0.28 : 0.36);
    float dist = length(d) * pxPer;
    float coreR = max(lerp(sky ? 1.05 : 0.45, sky ? 1.75 : 1.1, bright), 0.2);
    float core = exp(-(dist * dist) / (coreR * coreR * (sky ? 0.62 : 0.42)));
    float halo = 0.0;
    if (t > (sky ? 0.82 : 0.98)) halo = exp(-(dist * dist) / (sky ? 10.0 : 12.0)) * (sky ? 0.55 : 0.80);
    float3 bb = starColor(pow(hash12(cell + float2(9.0, 2.2)), 0.55));
    // Band-limited twinkle. Low stars scintillate more than stars overhead.
    float twN = vnoise(float2(time * (0.55 + n * 1.1), n * 37.0));
    float twAmp = lerp(0.08, 0.22, hash12(cell + float2(1.0, 5.0)));
    float tw = 1.0 - twAmp + twAmp * 2.0 * twN;
    float amp = lerp(1.0, tw, twinkle);
    float gain = sky ? (0.34 + 2.15 * bright) : (0.35 + 1.15 * bright);
    float inten = (core * gain + halo) * amp * density;
    col += bb * inten * (0.55 + 0.45 * depth) * tint;
}

void starLayer(inout float3 col, float2 p, float scale, float depth, float keep, float twinkle,
               float density, float3 tint, float field) {
    float2 q = (p + float2(lookX, -lookY) * (0.02 + 0.05 * depth)) * scale;
    starPoint(col, floor(q), frac(q) - 0.5, scale, depth, keep, twinkle, density, tint, height / scale, field);
}

float3 auroraRibbons(float px, float py, float t, float heightK, float raysK, float3 ca, float3 cb,
                     float surge, float aspect, float lift, float edgeOn) {
    float3 curtain = 0.0;
    raysK = saturate(raysK);
    [unroll] for (int layer = 0; layer < 2; layer++) {
        float fade = (layer == 0) ? 1.0 : 0.30;
        float xShift = (layer == 0) ? 0.0 : 3.1;
        float hScale = (layer == 0) ? 1.0 : 1.4;
        float rayContrast = (layer == 0) ? 1.0 : 0.5;
        float edgeBoost = (layer == 0 && edgeOn > 0.5) ? 1.0 : 0.0;
        float3 tint = (layer == 0) ? float3(1.0, 1.0, 1.0) : float3(0.8, 0.9, 1.1);
        float w1 = fbm2(float2(px * 0.38 + t * 0.02 + xShift, 0.4 + layer * 4.0));
        float w2 = fbm2(float2(px * 1.05 + 2.0 + layer * 3.0, 1.6 + layer));
        float qx = px + (w1 - 0.5) * 1.6 + (w2 - 0.5) * 0.40;
        float w1b = fbm2(float2((px + 0.02) * 0.38 + t * 0.02 + xShift, 0.4 + layer * 4.0));
        float w2b = fbm2(float2((px + 0.02) * 1.05 + 2.0 + layer * 3.0, 1.6 + layer));
        float qxb = (px + 0.02) + (w1b - 0.5) * 1.6 + (w2b - 0.5) * 0.40;
        float dqx = (qxb - qx) / 0.02;
        float gain = clamp(0.35 / max(abs(dqx), 0.05), 0.6, 2.5);
        gain = 0.55 + 0.55 * gain;
        float fold = fbm2(float2(qx * 0.7 + layer * 2.2, 0.3 + t * 0.01));
        // Keep the hem in the sky. The old 0.64-0.92 range sat on the horizon.
        float base = lerp(0.56, 0.38, heightK) - lift;
        base += (fold - 0.48) * 0.16;
        base = min(base, 0.58);
        float above = base - py;
        float hLen = lerp(0.06, 0.24, heightK) * hScale;
        float rx = qx + (px - 0.5 * aspect) * max(above, 0.0) * 0.35;
        float ray = 0.35 + 0.65 * pow(saturate(vnoise1(rx * 38.0)), 2.5) + 0.25 * vnoise1(rx * 110.0);
        ray = lerp(1.0, ray, raysK * rayContrast);
        float hs = hLen * (0.45 + 0.9 * ray);
        float body = smoothstep(-0.0025, 0.0015, above) * exp(-max(above, 0.0) / max(hs, 0.001));
        float hem = exp(-max(above, 0.0) / 0.0045) * edgeBoost;
        float knots = 0.6 + 0.8 * vnoise1(qx * 6.0);
        float intensity = body * (1.0 + 2.4 * hem) * knots * gain;
        float sheet = smoothstep(0.25, 0.65, fbm2(float2(qx * 0.32 + xShift, 0.7 + layer)));
        float tcol = above / max(hs, 0.001);
        float3 rgb = lerp(ca, cb, smoothstep(0.60, 1.20, tcol));
        rgb = min(rgb, float3(1.4, 1.4, 1.4));
        float inten = lerp(1.0, 0.25, saturate(smoothstep(1.0, 1.8, tcol)));
        float glow = exp(-abs(above) / 0.12) * 0.05;
        float pink = exp(-max(-above, 0.0) / 0.002) * (1.0 - smoothstep(-0.0025, 0.0015, above));
        pink *= smoothstep(0.8, 0.95, surge) * edgeBoost;
        float mask = sheet * fade;
        curtain += rgb * tint * intensity * inten * mask;
        curtain += ca * tint * glow * mask;
        curtain += float3(1.0, 0.35, 0.55) * pink * 0.45 * mask;
    }
    float w = fbm2(float2(px * 0.30 + 5.0, 1.2));
    float base3 = lerp(0.92, 0.64, heightK) - lift - 0.16 + (w - 0.5) * 0.20;
    float a3 = base3 - py;
    float i3 = smoothstep(-0.10, 0.06, a3) * exp(-max(a3, 0.0) / 0.16) * 0.06;
    curtain += ca * i3;
    return curtain;
}

float fractalRidge(float x, float base, float amp, float seed) {
    float h = ridged(x, 1.7, seed) * 0.46;
    h += ridged(x, 3.6, seed + 1.7) * 0.28;
    h += ridged(x, 7.8, seed + 4.0) * 0.16;
    h += ridged(x, 16.0, seed + 8.0) * 0.10;
    return base - h * amp;
}

float pineCover(float2 uv, float ground, float scale, float seed, float h0, float h1, float groveY,
                float presentAt, float shore) {
    float tx = uv.x * scale;
    float grove = fbm2(float2(uv.x * 2.3, groveY));
    float cov = 0.0;
    float px = width / scale;
    [unroll] for (int k = -1; k <= 1; k++) {
        float cell = floor(tx) + float(k);
        float fr = tx - cell;
        float hA = hash12(float2(cell, seed));
        float hB = hash12(float2(cell, seed + 17.0));
        float hC = hash12(float2(cell, seed + 4.0));
        float hD = hash12(float2(cell, seed + 29.0));
        float present = (hC > presentAt && grove > 0.30 && ground > shore - 0.08) ? 1.0 : 0.0;
        float lean = (hD - 0.5) * 0.05;
        float center = 0.50 + (hB - 0.5) * 0.20;
        float treeH = lerp(h0, h1, hB);
        float tt = (ground - uv.y) / max(treeH, 0.001);
        float dx = abs(fr - (center + lean * saturate(tt)));
        float tiers = lerp(7.0, 10.0, hA);
        float halfW = lerp(0.10, 0.20, hD);
        float tier = frac(tt * tiers);
        float flare = lerp(1.25, 0.15, smoothstep(0.0, 0.78, tier));
        float env = max(1.0 - tt, 0.0) * flare;
        float edge = saturate((halfW * env - dx) * px / 1.15);
        float on = (tt > 0.0 && tt < 1.0) ? present : 0.0;
        cov = max(cov, edge * on);
        float trunkH = treeH * 0.18;
        float trunkOn = (uv.y > ground && uv.y < ground + trunkH && abs(fr - center) < 0.035) ? present : 0.0;
        cov = max(cov, trunkOn);
    }
    return cov;
}

float4 PSStarfield(VSOut i) : SV_Target {
    float aspect = width / max(height, 1.0);
    float2 uv = i.uv;
    float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
    float travel = saturate(p0);
    float rot = time * speed * 0.012 * (1.0 - travel);
    float cs = cos(rot);
    float sn = sin(rot);
    p = float2(cs * p.x - sn * p.y, sn * p.x + cs * p.y);
    float diag = p.y * 0.95 + p.x * 0.78;
    float along = p.x * 0.95 - p.y * 0.78;
    float bandScale = max(1.0 - travel * 0.55, 0.42);
    int plates = (int)(padA + 0.5);
    // On a photographed night the full procedural veil reads as a brown fog.
    // Still night keeps a faint silver arm. Milky Way (density > 1.35) is stronger.
    if (plates != 0) bandScale *= (density > 1.35) ? 0.70 : 0.36;
    float3 col = float3(0.0, 0.0, 0.0);
    if (plates == 0) {
        float skyT = smoothstep(0.0, 0.9, uv.y);
        col = lerp(float3(0.004, 0.006, 0.018), float3(0.009, 0.013, 0.030), skyT);
        col += float3(0.005, 0.009, 0.016) * smoothstep(0.55, 1.0, uv.y);
    }
    // Wide gaussian, evaluated on every pixel. The old cheap>0.003 test cut this
    // glow on two straight diagonals while it was still bright.
    float env = exp(-(diag * diag) / (2.0 * 0.30 * 0.30));
    float coreA = exp(-((along - 0.15) * (along - 0.15)) / 0.28);
    float3 glowCol = lerp(float3(0.55, 0.66, 0.98), float3(0.92, 0.68, 0.40), coreA);
    if (plates != 0) glowCol = lerp(glowCol, glowCol * float3(0.70, 0.84, 1.12), 0.62);
    float3 glow = glowCol * (0.18 + 0.42 * coreA) * env * bandScale;
    if (quality > 0.5 && env > 0.35) {
        // Detail fades out before the branch, so skipping it leaves no edge.
        float fade = smoothstep(0.35, 0.62, env);
        float wobble = (fbm2(float2(along * 0.9, 2.2)) - 0.5) * 0.05;
        float diagW = diag - wobble;
        float widthB = 0.10 + 0.05 * fbm2(float2(along * 0.8, 4.0)) + 0.06 * coreA;
        float band = exp(-(diagW * diagW) / (2.0 * widthB * widthB));
        float2 dom = float2(along * 4.0 + fbm2(float2(along * 2.0, diagW * 3.0)), diagW * 9.0);
        float clouds = pow(saturate(fbm3(dom)), 1.6);
        float riftOff = 0.012 + (fbm2(float2(along * 1.2, 1.0)) - 0.5) * 0.03;
        float rift = exp(-((diagW - riftOff) * (diagW - riftOff)) / (2.0 * 0.018 * 0.018));
        float riftSpan = smoothstep(-0.6, -0.35, along) * (1.0 - smoothstep(0.15, 0.40, along));
        float fil = vnoise(float2(along * 2.5, diagW * 24.0));
        fil = 1.0 - abs(fil * 2.0 - 1.0);
        fil = fil * fil;
        float dust = saturate(rift * riftSpan * 0.85 + fil * 0.40 * band);
        float3 detail = glowCol * (0.22 + 0.48 * coreA) * (0.40 + 0.60 * clouds) * band * (1.0 - 0.70 * dust) * bandScale;
        float knots = smoothstep(0.78, 0.90, fbm2(float2(along * 3.0, diagW * 8.0)));
        detail += float3(1.0, 0.40, 0.48) * knots * 0.10 * band * bandScale;
        float h1 = hash12(floor(i.pos.xy * 0.75));
        float h2 = hash12(floor(i.pos.xy * 0.75) + float2(19.0, 3.0));
        float haze = (h1 > 1.0 - 0.38 * band) ? pow(h2, 2.4) * 0.28 * (1.0 - dust) : 0.0;
        detail += haze * glowCol * bandScale;
        float ext = lerp(1.0, 1.0 - 0.48 * dust * band, fade);
        // Dust lanes carved into the veil become light rays on a photograph.
        if (plates != 0) ext = lerp(1.0, ext, (density > 1.35) ? 0.22 : 0.0);
        glow *= ext;
        // Filaments and knots read as searchlight beams on a photograph. Keep a little dust on the Milky Way.
        float detailGain = (plates != 0) ? ((density > 1.35) ? 0.18 : 0.0) : 1.0;
        glow += detail * fade * detailGain;
    }
    // Shoulder on the glow only, so the warm core keeps rift detail and stars stay bright.
    glow = glow / (1.0 + glow * 0.90);
    col += glow;
    float skyKeep = 0.12 + 0.32 * exp(-(diag * diag) * 3.5);

    float peak = max(max(colorA.r, colorA.g), max(colorA.b, 0.001));
    float3 tint = colorA.rgb / peak;
    float twinkle = saturate(p2);
    if (travel > 0.25 && quality > 0.5) {
        float r = max(length(p), 0.001);
        float ang = atan2(p.y, p.x);
        float u = ang / 6.2831853 * 90.0;
        float v = log(r) * 18.0 - scroll * (1.4 / 0.085);
        float2 cell = floor(float2(u, v));
        float2 fr = frac(float2(u, v));
        float n = hash12(cell + float2(1.7, 4.0));
        float keep = clamp(0.45 * density, 0.05, 0.9);
        if (n > 1.0 - keep) {
            float t = saturate((n - (1.0 - keep)) / keep);
            float bright = pow(t, 4.0);
            float side = fr.x - (0.5 + (hash12(cell + float2(0.0, 8.0)) - 0.5) * 0.25);
            float alongS = fr.y - 0.72;
            float sidePx = abs(side * (6.2831853 / 90.0)) * r * height;
            float len = lerp(0.04, 0.85, saturate(r * 1.6)) * lerp(0.35, 1.0, travel);
            float streak = exp(-(sidePx * sidePx) / (1.15 * 1.15));
            streak *= exp(-max(-alongS, 0.0) / len);
            streak *= smoothstep(0.14, 0.0, max(alongS, 0.0));
            float3 bb = starColor(pow(hash12(cell + float2(3.0, 0.0)), 0.55));
            float head = smoothstep(0.14, 0.0, max(alongS, 0.0));
            bb = lerp(bb, bb * float3(0.75, 0.88, 1.15), head * 0.45);
            col += bb * streak * (0.55 + 2.4 * bright) * density * tint;
        }
    } else if (quality > 0.5) {
        starLayer(col, p, 34.0, 1.0, 0.22, twinkle, density, tint, 1.0);
        starLayer(col, p, 88.0, 0.62, skyKeep, twinkle, density, tint, 1.0);
    } else {
        starLayer(col, p, 40.0, 1.0, 0.16, twinkle, density, tint, 1.0);
    }

    float nebula = saturate(p1);
    if (plates == 0 && nebula > 0.08 && quality > 0.5) {
        float blob = exp(-(p.x + 0.05) * (p.x + 0.05) / 0.70 - (p.y + 0.02) * (p.y + 0.02) / 0.28);
        float blob2 = exp(-(p.x - 0.45) * (p.x - 0.45) / 0.35 - (p.y - 0.12) * (p.y - 0.12) / 0.16);
        float n1 = fbm2(p * 1.15 + float2(time * 0.008, -0.4));
        float n2 = fbm2(float2(p.x * 2.4 + 1.7, p.y * 1.8 + 0.6));
        float veil = smoothstep(0.36, 0.78, n1) * (0.40 + 0.60 * n2) * (blob + 0.65 * blob2) * nebula;
        float dust = smoothstep(0.62, 0.82, fbm2(p * 2.2 + float2(4.0, time * 0.004)));
        float3 ha = float3(0.82, 0.31, 0.42);
        float3 oiii = float3(0.25, 0.65, 0.63);
        float3 nebCol = lerp(ha, oiii, saturate(n2)) * 0.65 + tint * 0.25;
        col += nebCol * veil * (1.0 - 0.55 * dust) * 1.7;
    }

    if (travel < 0.45) {
        // One meteor per 20s slot, and only some slots fire, so they stay rare.
        float slot = floor(time / 20.0);
        float u = frac(time / 20.0);
        float pick = hash12(float2(slot, 3.3));
        if (pick > 0.67 && u < 0.10) {
            float2 start = float2(hash12(float2(slot, 1.7)) * 1.3 - 0.75, hash12(float2(slot, 4.2)) * 0.50 - 0.38);
            float2 dir = normalize(float2(0.86, 0.18 + 0.22 * hash12(float2(slot, 8.0))));
            float2 head = start + dir * (u / 0.10) * 0.72;
            float2 rel = p - head;
            float alongM = dot(rel, dir);
            float side = length(rel - dir * alongM);
            float sidePx = side * height;
            float alongPx = alongM * height;
            float behind = max(-alongPx, 0.0);
            float headG = exp(-(sidePx * sidePx + alongPx * alongPx) / (0.8 * 0.8));
            float wide = lerp(2.2, 0.55, saturate(behind / (height * 0.22)));
            float trail = exp(-(sidePx * sidePx) / (wide * wide));
            trail *= exp(-behind / (height * 0.22));
            trail *= smoothstep(height * 0.22, height * 0.01, behind);
            trail *= (alongPx < 0.8) ? 1.0 : 0.0;
            float life = u / 0.10;
            float brightM = lerp(0.55, 1.7, hash12(float2(slot, 9.1)));
            float fade = sin(pow(saturate(life), 0.55) * 3.14159265) * brightM;
            col += headG * fade * float3(1.15, 1.05, 0.90) + trail * fade * float3(1.05, 0.72, 0.42);
        }
    }
    if (plates != 0) {
        // Stars and the Milky Way stay in the empty sky, behind the ridges.
        float3 sky = plateLayer(0, i.uv).rgb + toLin(tonemap(col));
        return float4(coverPlates(sky, i.uv, 0.0), 1.0);
    }
    return float4(toLin(tonemap(col)), 1.0);
}

float s1080();
float groundPx(float x, float fracY, float amp, float len, float seed);
float pineCard(float2 px, float gFrac, float amp, float len, float cell, float seed, float h0, float h1, float edgeMode, float b0, float b1);

float4 PSAurora(VSOut i) : SV_Target {
    float2 uv = i.uv + float2(lookX, -lookY) * 0.12;
    float aspect = width / max(height, 1.0);
    float2 pxy = float2(uv.x * aspect, uv.y);
    float t = time * speed;
    float skyT = smoothstep(0.0, 0.88, uv.y);
    int plates = (int)(padA + 0.5);
    if (plates != 0) {
        // Curtain and stars in the sky only. Mountains cover them. Ice takes a little of that light.
        float3 fx = float3(0.0, 0.0, 0.0);
        if (p2 > 0.02 || quality > 0.5) {
            float2 sp = float2((i.uv.x - 0.5) * aspect, i.uv.y - 0.5);
            starLayer(fx, sp, 70.0, 0.7, 0.045, 0.45, 0.65, float3(0.85, 0.90, 1.0), 1.0);
        }
        float surgeP = 0.72 + 0.28 * vnoise(float2(t * 0.045, 2.2));
        float liftP = (p3 > 0.5) ? 0.10 : 0.0;
        // No hard hem. Fade the curtain before the ridgeline so the photo sky stays the picture.
        float skyRoom = 1.0 - smoothstep(0.16, 0.44, i.uv.y);
        fx += auroraRibbons(pxy.x, pxy.y, t, p0, p1, colorA.rgb, colorB.rgb, surgeP, aspect, liftP, 0.0) * (0.40 * skyRoom);
        float3 sky = plateLayer(0, i.uv).rgb + toLin(tonemap(fx));
        return float4(coverPlates(sky, i.uv, 1.0), 1.0);
    }
    float3 col = lerp(float3(0.002, 0.003, 0.008), float3(0.008, 0.010, 0.020), skyT);
    if (p2 > 0.02 || quality > 0.5) {
        float2 sp = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
        starLayer(col, sp, 70.0, 0.7, 0.045, 0.45, 0.65, float3(0.85, 0.90, 1.0), 1.0);
    }
    float surge = 0.72 + 0.28 * vnoise(float2(t * 0.045, 2.2));
    float lift = (p3 > 0.5) ? 0.10 : 0.0;
    col += auroraRibbons(pxy.x, pxy.y, t, p0, p1, colorA.rgb, colorB.rgb, surge, aspect, lift, 1.0);
    if (plates == 0 && p3 < 0.5 && i.uv.y > 0.78) {
        float2 ap = i.uv * float2(width, height);
        float gnd = groundPx(ap.x, 0.88, 16.0, 460.0, 3.1);
        float hill = smoothstep(gnd - 14.0 * s1080(), gnd + 3.0 * s1080(), ap.y);
        float pines = pineCard(ap, 0.88, 16.0, 460.0, 110.0, 5.0, 0.035, 0.08, 0.0, 6.0, 16.0);
        float3 groundCol = float3(0.006, 0.008, 0.012);
        col = lerp(col, groundCol, hill * 0.92);
        col = lerp(col, float3(0.003, 0.005, 0.008), saturate(pines) * (1.0 - hill));
        col += colorA.rgb * saturate(pines) * (1.0 - hill) * 0.04;
    }
    if (plates == 0 && p3 > 0.5 && quality > 0.5) {
        float shore = 0.66 + (fbm2(float2(uv.x * 1.6, 11.0)) - 0.5) * 0.012;
        float far = fractalRidge(uv.x, shore + 0.02, 0.34, 1.0);
        float near = fractalRidge(uv.x, shore, 0.16, 6.5);
        float aa = 1.35 / max(height, 1.0);
        if (uv.y < shore) {
            float trees = 0.0;
            if (uv.y > shore - 0.28) {
                trees = pineCover(uv, far, 40.0, 3.0, 0.016, 0.040, 1.2, 0.62, shore);
                trees = max(trees, pineCover(uv, near, 22.0, 30.0, 0.035, 0.100, 6.5, 0.08, shore));
                trees = max(trees, pineCover(uv, shore, 12.0, 80.0, 0.045, 0.120, 9.0, 0.05, shore));
                float treeline = smoothstep(0.0, 0.015, near - uv.y) * smoothstep(0.04, 0.01, near - uv.y);
                treeline *= (fbm2(float2(uv.x * 8.0, 2.0)) > 0.35) ? 1.0 : 0.0;
                trees = max(trees, treeline);
            }
            if (uv.y >= far) {
                float nearA = smoothstep(near - aa, near + aa, uv.y);
                float tex = 0.8 + 0.2 * fbm2(float2(uv.x * 40.0, uv.y * 40.0));
                float haze = saturate((uv.y - (far - 0.02)) / 0.08);
                float3 farCol = lerp(float3(0.018, 0.030, 0.028), float3(0.008, 0.012, 0.014), haze) * tex;
                float3 rock = lerp(farCol, float3(0.003, 0.004, 0.005), nearA);
                float3 behind = col;
                col = lerp(rock, float3(0.0012, 0.0012, 0.0012), saturate(trees));
                float d = (far - uv.y) / max(aa, 0.0001);
                float rim = exp(-((d - 1.2) * (d - 1.2)) / 7.0) * ((d > -0.2) ? 1.0 : 0.0);
                rim *= (1.0 - nearA) * (1.0 - saturate(trees)) * saturate(behind.g * 6.0);
                col += (colorA.rgb * 0.55 + colorB.rgb * 0.25) * rim * 0.35;
            }
        } else {
            float my = shore * 2.0 - uv.y;
            float depth = max(uv.y - shore, 0.0);
            if (my > far) {
                col = float3(0.004, 0.004, 0.004);
            } else {
                float xoff = (vnoise(float2(uv.x * 4.0, uv.y * 60.0)) - 0.5) * 0.015 * (depth / 0.2);
                float3 refl = auroraRibbons(uv.x * aspect + xoff, my, t, p0, p1 * 0.25, colorA.rgb, colorB.rgb,
                                             surge, aspect, 0.10, 0.0);
                float streaks = 0.55 + 0.45 * vnoise(float2(uv.x * 1.5, uv.y * 140.0));
                col = refl * streaks * exp(-depth / 0.22) * 0.85;
            }
        }
    }
    return float4(toLin(tonemap(col)), 1.0);
}

// Tiered conifers on a soil line that reaches the bottom of the frame.
// Sizes are pixels at 1080p, scaled by height. Motion is periodic over 3600s.

float s1080() { return height / 1080.0; }

float groundPx(float x, float fracY, float amp, float len, float seed) {
    float s = max(s1080(), 0.001);
    float p = x / s;
    float L = max(len, 48.0);
    float w = sin(p * (6.2831853 / L) + seed) * 0.62
            + sin(p * (6.2831853 / (L * 0.37)) + seed * 2.1) * 0.28
            + sin(p * (6.2831853 / 52.0) + seed * 3.7) * 0.22;
    return clamp(fracY * height + amp * s * w, 8.0, height - 6.0);
}

float grassFringe(float2 px, float fracY, float amp, float len, float seed, float shortH, float tallH) {
    float s = s1080();
    float gHere = groundPx(px.x, fracY, amp, len, seed);
    if (px.y < gHere - tallH * s - 4.0 || px.y > gHere + 2.0) return 0.0;
    float cell = max(13.0 * s, 5.0);
    float cov = 0.0;
    float col = floor(px.x / cell);
    [unroll] for (int k = -1; k <= 1; k++) {
        float cid = col + float(k);
        if (hash12(float2(cid, seed + 3.0)) > 0.28) {
            [unroll] for (int b = 0; b < 2; b++) {
                float hs = hash12(float2(cid, seed + 40.0 + float(b) * 2.3));
                if (hs > 0.08) {
                    float u = lerp(0.28, 0.72, hash12(float2(cid, seed + 51.0 + float(b))));
                    float cx = (cid + u) * cell;
                    float root = groundPx(cx, fracY, amp, len, seed);
                    float bh = lerp(shortH, tallH, pow(saturate(hs), 1.7)) * s;
                    float bw = lerp(1.05, 2.05, hash12(float2(cid, seed + 60.0 + float(b)))) * s;
                    float lean = (hash12(float2(cid, seed + 70.0 + float(b))) - 0.5) * 0.85;
                    float t = (root - px.y) / max(bh, 1.0);
                    if (t > 0.0 && t < 1.0) {
                        float half = bw * pow(saturate(1.0 - t), 1.45);
                        float dx = abs(px.x - cx - lean * (root - px.y));
                        cov = max(cov, saturate(half - dx + 0.5));
                    }
                }
            }
        }
    }
    return cov;
}

// edgeMode: 0 grass fringe, 1 soft snow drift, 2 hard fill.
float pineCard(float2 px, float gFrac, float amp, float len, float cell, float seed, float h0, float h1, float edgeMode, float b0, float b1) {
    float s = s1080();
    float g = groundPx(px.x, gFrac, amp, len, seed + 0.2);
    float cov;
    if (edgeMode > 0.5 && edgeMode < 1.5) {
        float feather = max(8.0 * s, 2.0);
        cov = smoothstep(g - feather, g + feather * 0.30, px.y);
    } else {
        cov = px.y + 0.5 >= g ? 1.0 : 0.0;
        if (edgeMode < 0.5) cov = max(cov, grassFringe(px, gFrac, amp, len, seed + 0.2, b0, b1));
    }
    float cellPx = max(cell * s, 4.0);
    float col = floor(px.x / cellPx);
    [unroll] for (int k = -1; k <= 1; k++) {
        float cid = col + float(k);
        [unroll] for (int ti = 0; ti < 2; ti++) {
            float hs = hash12(float2(cid, seed + float(ti) * 3.0));
            float h2 = hash12(float2(cid, seed + 5.0 + float(ti)));
            float gate = (ti == 0) ? 0.08 : 0.55;
            if (hs > gate) {
                float u = lerp(0.08, 0.92, hs);
                if (ti == 1) u = frac(u + 0.37);
                float center = (cid + u) * cellPx;
                float hh = (h0 + (h1 - h0) * pow(saturate(h2), 0.7)) * height;
                if (hash12(float2(cid, seed + 19.0)) > 0.82) hh *= 1.28;
                float root = g + (hash12(float2(cid, seed + 4.0)) - 0.5) * 10.0 * s + 0.02 * hh;
                float t = (root - px.y) / max(hh, 1.0);
                if (t > 0.0 && t < 1.0) {
                    float base = hh * lerp(0.15, 0.22, hs);
                    float prof = base * pow(saturate(1.0 - t), 0.85);
                    float notch = frac(t * lerp(7.0, 12.0, hs));
                    prof *= lerp(0.42, 1.0, pow(saturate(1.0 - notch), 1.6));
                    prof += base * 0.08 * (vnoise(float2(px.y / 6.0, seed + cid)) - 0.5);
                    if (t > 0.94) prof = min(prof, 1.6);
                    cov = max(cov, saturate(prof - abs(px.x - center) + 0.5));
                }
            }
        }
    }
    return cov;
}

// pattern in 0..1 picks the blink. Periods divide 3600s so the hour wrap is seamless.
float blinkEnv(float phase, float pattern) {
    float period = pattern < 0.55 ? 6.0 : (pattern < 0.80 ? 5.0 : 8.0);
    float u = frac(time / 3600.0 * (3600.0 / period) + phase);
    if (pattern < 0.55) {
        // A long glow, still dark more often than it is lit.
        return smoothstep(0.0, 0.03, u) * (1.0 - smoothstep(0.42, 0.55, u));
    }
    if (pattern < 0.80) {
        float a = smoothstep(0.04, 0.07, u) * (1.0 - smoothstep(0.14, 0.18, u));
        float b = smoothstep(0.24, 0.27, u) * (1.0 - smoothstep(0.34, 0.40, u));
        return saturate(a + b);
    }
    return smoothstep(0.0, 0.025, u) * (1.0 - smoothstep(0.28, 0.40, u));
}

void addFlyLayer(inout float3 col, inout float maskA, float2 px, float y0, float y1,
                 float count, float3 tint, int kind) {
    float s = s1080();
    px.x -= lookX * ((kind == 0) ? 44.0 : ((kind == 1) ? 133.0 : 267.0));
    float spK = speed / 0.32;
    float ampK = clamp(spK, 0.25, 1.5);
    float kMul = max(1.0, round(spK / 1.5));
    float n = max(count * max(density, 0.05) * s, 1.0);
    float baseAx = (kind == 1) ? 20.0 : ((kind == 2) ? 36.0 : 10.0);
    float baseAy = (kind == 1) ? 14.0 : ((kind == 2) ? 16.0 : 7.0);
    float ax = baseAx * s * ampK;
    float ay = baseAy * s * ampK;
    float bMax = ((kind == 0) ? 6.0 : ((kind == 1) ? 10.0 : 22.0)) * s;
    float reachY = 3.0 * bMax + ay + 6.0 * s + 1.0;
    float reachX = 3.0 * bMax + ax + 1.0;
    float bandH = max(y1 - y0, 8.0);
    if (px.y < y0 - reachY || px.y > y1 + reachY) return;
    float cols = (kind == 2) ? n : max(1.0, sqrt(n * width / max(height, 1.0)));
    float rows = (kind == 2) ? 1.0 : max(1.0, ceil(n / max(cols, 1.0)));
    rows = min(rows, max(1.0, floor(bandH * 1.15 / max(reachY, 1.0))));
    cols = max(cols, ceil(n / max(rows * 0.85, 1.0)));
    cols = min(cols, max(1.0, floor(width * 1.15 / max(reachX, 1.0))));
    float cw = width / max(cols, 1.0);
    float ch = bandH / max(rows, 1.0);
    float occ = min(0.92, n / max(cols * rows, 1.0));
    float2 cell = float2(floor(px.x / cw), floor((px.y - y0) / ch));
    [unroll] for (int jy = -1; jy <= 1; jy++) {
        [unroll] for (int jx = -1; jx <= 1; jx++) {
            float2 ix = cell + float2((float)jx, (float)jy);
            float hs = hash12(ix + float2(0.0, 3.1));
            if (hs > 1.0 - occ) {
                float2 j = hash22(ix + float2(2.0, 1.0));
                float2 c = float2((ix.x + 0.15 + 0.7 * j.x) * cw, y0 + (ix.y + 0.15 + 0.7 * j.y) * ch);
                float ang = 6.2831853 * time / 3600.0;
                float k1 = (180.0 + floor(hs * 200.0)) * kMul;
                float k2 = (260.0 + floor(hash12(ix + float2(0.0, 9.0)) * 180.0)) * kMul;
                float pattern = hash12(ix + float2(0.0, 4.0));
                float env = blinkEnv(hs, pattern);
                c.x += ax * sin(ang * k1 + hs * 6.0);
                c.y += ay * sin(ang * k2 + hs * 4.0);
                c.y -= (pattern > 0.80 ? 14.0 : 6.0) * s * env;
                float2 d = px - c;
                float dist = length(d);
                float shade = 0.0;
                float3 glow = lerp(tint, float3(1.0, 0.965, 0.784), 0.55 * env);
                float level = 0.45;
                if (kind == 2) {
                    float rad = lerp(11.0, 20.0, hs) * s;
                    float edge = 3.5 * s;
                    float disc = smoothstep(rad + edge, max(rad - edge, 0.0), dist);
                    float rim = smoothstep(rad, rad - 2.2 * s, dist) * smoothstep(rad - 5.5 * s, rad - 2.2 * s, dist);
                    shade = (disc * 0.20 + rim * 0.07) * env;
                    glow = lerp(tint * 0.8, float3(1.0, 0.965, 0.784), 0.35);
                    level = 1.0;
                } else {
                    float coreR = ((kind == 0) ? lerp(0.7, 1.05, hs) : lerp(1.2, 1.9, hs)) * s;
                    float bloom = ((kind == 0) ? lerp(4.0, 6.0, hs) : lerp(7.0, 10.0, hs)) * s;
                    float r0 = ((kind == 0) ? 2.6 : 3.6) * s;
                    float peak = (kind == 0) ? 1.05 : 1.7;
                    float gain = (kind == 0) ? 0.28 : 0.42;
                    // Brighter lamp on the photograph. Stay inside the 3x3 search so the halo is not cut.
                    if ((int)(padA + 0.5) != 0) {
                        bloom *= 1.5;
                        r0 *= 1.35;
                        gain *= 2.2;
                    }
                    float core = exp(-dist * dist / max(coreR * coreR, 0.2));
                    float halo = pow(1.0 / (1.0 + (dist / r0) * (dist / r0)), 1.5);
                    float window = exp(-dist * dist / max(bloom * bloom, 0.4));
                    shade = (core * peak + halo * window * gain) * env;
                    glow = lerp(tint, float3(1.0, 0.965, 0.784), 0.62 * env);
                }
                float pad = max(8.0 * s, 2.0);
                float edgeFade = smoothstep(y0 - pad - 12.0 * s, y0 - pad, c.y)
                               * (1.0 - smoothstep(y1 + pad, y1 + pad + 12.0 * s, c.y));
                shade *= edgeFade;
                col += glow * shade;
                float mark = shade > 0.05 ? 1.0 : 0.0;
                maskA = max(maskA, mark * level);
            }
        }
    }
}

void snowDisc(inout float3 col, float2 px, float cellX0, float cellY0, float d0, float d1,
              float blur0, float a0, float a1, float fall, float sway, float freq, float stretch,
              float count, float seed, float3 tint, float extraBlur) {
    float s = s1080();
    px.x -= lookX * (cellX0 < 80.0 ? 44.0 : (cellX0 < 200.0 ? 133.0 : 267.0));
    float cellX = cellX0 * s;
    float cellY = cellY0 * s;
    float rate = fall * (speed / 0.40) * s;
    float qy = cellY / 3600.0;
    float fallQ = round(rate / max(qy, 1e-5)) * qy;
    float windQ = round((p0 * fallQ * 0.7) / max(cellX / 3600.0, 1e-5)) * (cellX / 3600.0);
    float shiftX = windQ * time;
    float originX = floor((px.x - shiftX) / cellX);
    float cells = max((1920.0 / cellX0) * (1080.0 / cellY0), 1.0);
    float keep = saturate(count * max(density, 0.05) * s / cells);
    [unroll] for (int ox = -1; ox <= 1; ox++) {
        float colX = originX + float(ox);
        // The old jitter scaled the whole fall distance, so flakes walked out of
        // the one row that looks them up and vanished after about a minute.
        float jit = 1.0 + 0.2 * (hash12(float2(colX, seed + 3.0)) - 0.5);
        float fallC = round(rate * jit / max(qy, 1e-5)) * qy;
        float shiftC = fallC * time;
        float rowY = floor((px.y - shiftC) / cellY);
        float2 cid = float2(colX, rowY);
        float hs = hash12(cid + float2(2.0, seed));
        if (hs > 1.0 - max(keep, 0.02)) {
            float2 j = hash22(cid + float2(seed, 0.4));
            float cycles = max(1.0, round(freq * 3600.0));
            float swayX = sway * s * sin(6.2831853 * time / 3600.0 * cycles + hs * 6.2);
            float2 c = float2((cid.x + 0.5 + (j.x - 0.5) * 0.3) * cellX + shiftX + swayX,
                              (cid.y + 0.5 + (j.y - 0.5) * 0.25) * cellY + shiftC);
            float diam = lerp(d0, d1, hs) * s;
            float rad = diam * 0.5;
            float blur = max((blur0 + extraBlur) * s, 0.45 * s);
            float2 d = px - c;
            d.y /= max(stretch, 0.2);
            float dist = length(d);
            float alpha = lerp(a0, a1, hash12(cid + float2(0.0, 8.0)));
            float3 shade = tint * lerp(0.92, 1.0, hs);
            float edge = smoothstep(rad + blur, max(rad - blur, 0.0), dist);
            float core = exp(-dist * dist / max(rad * rad * 0.28, 0.2));
            float a = edge * alpha * lerp(0.62, 1.0, core);
            col = lerp(col, shade, saturate(a));
        }
    }
}

float4 PSFireflies(VSOut i) : SV_Target {
    float2 px = i.uv * float2(width, height);
    float s = s1080();
    int style = (direction < 0.5) ? 0 : ((direction < 1.5) ? 1 : 2);
    int plates = (int)(padA + 0.5);
    if (plates != 0 && p3 > -0.5) {
        // Dusk plate is warm. Pull the lamp color toward that amber so the bugs match the light.
        float3 glow = max(colorA.rgb, float3(0.02, 0.02, 0.02));
        glow = lerp(glow, float3(1.0, 0.58, 0.20), 0.62);
        float3 fx = float3(0.0, 0.0, 0.0);
        float flyA = 0.0;
        // The open gap is the dark treeline and the near meadow. Closer grass covers the bottom of the band.
        if (quality > 0.5) addFlyLayer(fx, flyA, px, height * 0.50, height * 0.64, 18.0, glow, 0);
        addFlyLayer(fx, flyA, px, height * 0.54, height * 0.72, 20.0, glow, 1);
        if (style != 1) addFlyLayer(fx, flyA, px, height * 0.60, height * 0.78, 3.0, glow, 2);
        float3 col = ((plates & 1) != 0) ? plateLayer(0, i.uv).rgb : float3(0.02, 0.015, 0.012);
        overPlate(col, 1, i.uv);
        overPlate(col, 2, i.uv);
        col += toLin(tonemap(fx));
        overPlate(col, 3, i.uv);
        return float4(col, 1.0);
    }
    float3 col;
    float gFar, gMid, gNear, aFar, aMid, aNear, cellF, cellM, cellN;
    float3 cFar, cMid, cNear, horizon;
    float mistA, exposure;
    float starN = 0.0;
    if (style == 1) {
        col = lerp(float3(0.008, 0.016, 0.028), float3(0.022, 0.045, 0.055), smoothstep(0.0, 0.75, i.uv.y));
        gFar = 0.52; gMid = 0.74; gNear = 0.84;
        aFar = 30.0; aMid = 36.0; aNear = 20.0;
        cellF = 84.0; cellM = 140.0; cellN = 280.0;
        cFar = float3(0.012, 0.022, 0.026); cMid = float3(0.004, 0.008, 0.010); cNear = float3(0.004, 0.006, 0.008);
        horizon = float3(0.012, 0.028, 0.032);
        mistA = 0.28; exposure = 2.4; starN = 0.0;
    } else if (style == 2) {
        col = lerp(float3(0.004, 0.008, 0.035), float3(0.22, 0.10, 0.05), smoothstep(0.35, 0.78, i.uv.y));
        gFar = 0.68; gMid = 0.76; gNear = 0.90;
        aFar = 16.0; aMid = 18.0; aNear = 12.0;
        cellF = 90.0; cellM = 280.0; cellN = 260.0;
        cFar = float3(0.10, 0.09, 0.15); cMid = float3(0.018, 0.028, 0.020); cNear = float3(0.004, 0.005, 0.005);
        horizon = float3(0.58, 0.32, 0.16);
        mistA = 0.12; exposure = 1.9; starN = 170.0;
    } else {
        col = lerp(float3(0.010, 0.016, 0.09), float3(0.55, 0.16, 0.07), smoothstep(0.45, 0.78, i.uv.y));
        gFar = 0.58; gMid = 0.84; gNear = 0.88;
        aFar = 34.0; aMid = 40.0; aNear = 24.0;
        cellF = 86.0; cellM = 150.0; cellN = 280.0;
        cFar = float3(0.022, 0.017, 0.040); cMid = float3(0.006, 0.005, 0.013); cNear = float3(0.005, 0.005, 0.010);
        horizon = float3(0.80, 0.30, 0.10);
        mistA = 0.18; exposure = 2.20; starN = 75.0;
    }
    float gy = groundPx(px.x, gFar, aFar, 480.0, 2.2);
    float band = exp(-pow((px.y - (gy - 0.02 * height)) / height, 2.0) / 0.001);
    col += horizon * band * (style == 1 ? 0.18 : 0.55);
    float b0 = style == 2 ? 8.0 : (style == 1 ? 6.0 : 7.0);
    float b1 = style == 2 ? 28.0 : (style == 1 ? 22.0 : 26.0);
    float farC = pineCard(px, gFar, aFar, 480.0, cellF, 2.0, style == 2 ? 0.05 : 0.12, style == 2 ? 0.12 : (style == 1 ? 0.28 : 0.24), 0.0, b0, b1);
    b0 = style == 2 ? 10.0 : (style == 1 ? 8.0 : 10.0);
    b1 = style == 2 ? 36.0 : (style == 1 ? 30.0 : 38.0);
    float midC = pineCard(px, gMid, aMid, 340.0, cellM, 6.5, style == 2 ? 0.055 : 0.22, style == 2 ? 0.14 : (style == 1 ? 0.42 : 0.46), 0.0, b0, b1);
    b0 = style == 2 ? 14.0 : (style == 1 ? 10.0 : 12.0);
    b1 = style == 2 ? 70.0 : (style == 1 ? 40.0 : 52.0);
    float nearC = pineCard(px, gNear, aNear, 260.0, cellN, 11.0, style == 1 ? 0.28 : (style == 2 ? 0.06 : 0.16), style == 1 ? 0.48 : (style == 2 ? 0.12 : 0.34), 0.0, b0, b1);
    float3 field = col;
    if (style == 2) {
        float t = saturate((i.uv.y - 0.56) / 0.44);
        float3 hi = pow(float3(138.0, 148.0, 116.0) / 255.0, 2.2);
        float3 mid = pow(float3(70.0, 84.0, 60.0) / 255.0, 2.2);
        float3 lo = pow(float3(22.0, 30.0, 20.0) / 255.0, 2.2);
        field = lerp(lerp(hi, mid, saturate(t * 1.35)), lo, saturate((t - 0.32) / 0.68));
    }
    float gFarY = groundPx(px.x, gFar, aFar, 480.0, 2.2);
    float gMidY = groundPx(px.x, gMid, aMid, 340.0, 6.7);
    float gNearY = groundPx(px.x, gNear, aNear, 260.0, 11.2);
    float3 farShade = (style == 2 && px.y >= gFarY) ? field : cFar * (style == 2 ? 1.0 : 1.6);
    float3 midShade = (style == 2) ? field : cMid;
    float3 nearShade = (style == 2 && px.y < gNearY) ? pow(float3(18.0, 24.0, 16.0) / 255.0, 2.2) : ((style == 2) ? field : cNear);
    col = lerp(col, farShade, saturate(farC));
    col = lerp(col, midShade, saturate(midC));
    col = lerp(col, nearShade, saturate(nearC));
    float mist = smoothstep(gy - 40.0 * s, gy + 10.0 * s, px.y) * mistA * saturate(p0);
    if (quality > 0.5) mist *= 0.65 + 0.35 * vnoise(float2(px.x / (180.0 * s) + time * 0.02, 0.4));
    col = lerp(col, float3(0.18, 0.20, 0.28), saturate(mist));
    float flyA = 0.0;
    float3 glow = max(colorA.rgb, float3(0.02, 0.02, 0.02));
    if (quality > 0.5) addFlyLayer(col, flyA, px, height * (gFar - 0.18), height * (gFar + 0.02), style == 1 ? 16.0 : 18.0, glow, 0);
    addFlyLayer(col, flyA, px, height * (gMid - 0.16), height * (gMid + 0.02), style == 1 ? 18.0 : 18.0, glow, 1);
    if (style != 1) addFlyLayer(col, flyA, px, height * 0.55, height * 0.92, 2.0, glow, 2);
    if (p1 > 0.02 && starN > 1.0 && i.uv.y < (style == 2 ? 0.42 : 0.50)) {
        float cell = 70.0 * s;
        float2 st = floor(px / cell);
        float sn = hash12(st + float2(4.0, 8.0));
        if (sn > 1.0 - saturate(starN * s * s / 8.0)) {
            float2 sj = hash22(st);
            float2 sc = (st + sj) * cell;
            float sd = length(px - sc);
            col += float3(0.75, 0.82, 1.0) * exp(-sd * sd / 1.2) * lerp(0.4, 1.3, sn);
        }
    }
    col *= exposure;
    if (p3 < -0.5) return float4(farC, midC, nearC, flyA);
    return float4(toLin(tonemap(col)), 1.0);
}

float4 PSSnow(VSOut i) : SV_Target {
    float2 px = i.uv * float2(width, height);
    float s = s1080();
    float backdrop = p1;
    float3 col = float3(0.0, 0.0, 0.0);
    float farC = 0.0, midC = 0.0, nearC = 0.0;
    float frameM = 0.0, glassM = 0.0, frostM = 0.0, sillM = 0.0;
    float3 flake = max(colorA.rgb, float3(0.2, 0.2, 0.2));
    float3 ambient = float3(0.49, 0.56, 0.70);
    int plates = (int)(padA + 0.5);
    if (plates != 0 && backdrop < 1.5 && p3 > -0.5) {
        // Moonlit snow on the plate is cool white, not the dark procedural ambient.
        float3 nearTint = lerp(saturate(flake), float3(0.93, 0.95, 1.0), 0.72);
        float3 farTint = nearTint * float3(0.70, 0.78, 0.92);
        col = ((plates & 1) != 0) ? plateLayer(0, i.uv).rgb : float3(0.01, 0.015, 0.03);
        overPlate(col, 1, i.uv);
        if (quality > 0.5) {
            snowDisc(col, px, 52.0, 44.0, 1.4, 2.2, 0.55, 0.40, 0.55, 32.0, 4.5, 0.70, 1.0, 900.0, 1.0, farTint, 0.0);
        }
        overPlate(col, 2, i.uv);
        overPlate(col, 3, i.uv);
        snowDisc(col, px, 96.0, 84.0, 3.2, 5.2, 0.9, 0.62, 0.82, 64.0, 12.0, 0.50, 1.0, 340.0, 2.0, nearTint, 0.0);
        snowDisc(col, px, 180.0, 160.0, 6.0, 9.0, 1.3, 0.75, 0.90, 112.0, 22.0, 0.35, 1.28, 80.0, 3.0, nearTint, 0.0);
        snowDisc(col, px, 520.0, 480.0, 18.0, 30.0, 4.0, 0.10, 0.18, 200.0, 36.0, 0.25, 1.25, 5.0, 4.0, nearTint, 0.0);
        return float4(col, 1.0);
    }
    if (backdrop < 0.5) {
        col = lerp(float3(0.010, 0.025, 0.070), float3(0.04, 0.08, 0.14), i.uv.y) * 2.4;
        float2 moon = px - float2(0.72 * width, 0.22 * height);
        col = lerp(col, float3(0.34, 0.46, 0.68), exp(-dot(moon, moon) / (2.0 * pow(0.18 * height, 2.0))) * 0.55);
        float moonR = 17.0 * s;
        float disc = smoothstep(moonR + 1.6 * s, moonR - 1.2 * s, length(moon));
        col = lerp(col, float3(0.82, 0.86, 0.93), disc * 0.72);
        float g = groundPx(px.x, 0.80, 22.0, 480.0, 4.0);
        float tips = pineCard(px, 0.80, 22.0, 480.0, 150.0, 4.2, 0.02, 0.045, 2.0, 0.0, 0.0);
        if (px.y < g - 1.0) col = lerp(col, float3(0.012, 0.020, 0.036) * 2.3, saturate(tips) * 0.72);
        float feather = max(12.0 * s, 2.0);
        float hill = smoothstep(g - feather, g + feather * 0.2, px.y);
        float3 field = lerp(float3(0.03, 0.05, 0.10), float3(0.07, 0.10, 0.17), saturate((i.uv.y - 0.76) / 0.24)) * 2.2;
        col = lerp(col, field, hill);
        ambient = float3(0.46, 0.54, 0.68);
    } else if (backdrop < 1.5) {
        col = lerp(float3(0.006, 0.010, 0.022), float3(0.020, 0.038, 0.075), i.uv.y) * 3.8;
        farC = pineCard(px, 0.60, 24.0, 420.0, 80.0, 2.0, 0.12, 0.26, 1.0, 0.0, 0.0);
        col = lerp(col, float3(0.024, 0.042, 0.078) * 3.2, saturate(farC));
        midC = pineCard(px, 0.74, 32.0, 420.0, 130.0, 6.0, 0.20, 0.40, 1.0, 0.0, 0.0);
        col = lerp(col, float3(0.008, 0.014, 0.028) * 3.2, saturate(midC));
        nearC = pineCard(px, 0.90, 26.0, 280.0, 240.0, 11.0, 0.28, 0.50, 1.0, 0.0, 0.0);
        col = lerp(col, float3(0.003, 0.004, 0.007) * 3.2, saturate(nearC));
        float g = groundPx(px.x, 0.90, 26.0, 280.0, 11.2);
        float feather = max(11.0 * s, 2.0);
        float floor = smoothstep(g - feather, g + feather * 0.2, px.y);
        float crest = exp(-pow((px.y - g) / max(7.0 * s, 1.0), 2.0));
        float3 field = lerp(float3(0.04, 0.07, 0.12), float3(0.12, 0.17, 0.26), saturate((i.uv.y - 0.86) / 0.14)) * 3.0;
        col = lerp(col, field * (1.0 + 0.18 * crest), floor);
        ambient = float3(0.52, 0.60, 0.72);
    } else {
        float3 lamp = float3(1.0, 0.34, 0.08);
        float d = length(px - float2(-0.10 * width, 0.95 * height));
        float lampA = 0.30 / (1.0 + pow(d / (0.45 * height), 2.0));
        col = lerp(float3(0.006, 0.003, 0.002), float3(0.022, 0.010, 0.005), saturate(lampA / 0.30));
        col += lamp * lampA;
        float ow = 1240.0 * s, oh = 820.0 * s, frameW = 34.0 * s, mul = 12.0 * s;
        float2 c = float2(0.52 * width, 0.47 * height);
        float x0 = c.x - ow * 0.5, y0 = c.y - oh * 0.5, x1 = c.x + ow * 0.5, y1 = c.y + oh * 0.5;
        bool opening = px.x >= x0 && px.x <= x1 && px.y >= y0 && px.y <= y1;
        bool outer = px.x >= x0 - frameW && px.x <= x1 + frameW && px.y >= y0 - frameW && px.y <= y1 + frameW + 60.0 * s;
        float paneW = ow * 0.5, paneH = oh / 3.0;
        bool mullion = opening && (abs((px.x - x0) - paneW) < mul * 0.5 || abs((px.y - y0) - paneH) < mul * 0.5 || abs((px.y - y0) - 2.0 * paneH) < mul * 0.5);
        bool frame = (outer && !opening) || mullion;
        bool glass = opening && !mullion;
        bool sill = px.x >= x0 - frameW && px.x <= x1 + frameW && px.y >= y1 && px.y <= y1 + 60.0 * s;
        frameM = frame ? 1.0 : 0.0;
        glassM = glass ? 1.0 : 0.0;
        float left = saturate((c.x - px.x) / (0.62 * ow) + 0.05);
        float3 wood = lerp(float3(0.026, 0.012, 0.006), float3(0.14, 0.055, 0.018), saturate(0.22 + 0.70 * left + lampA * 1.6));
        float shade = 1.0;
        float lip = 2.2 * s;
        if (frame && (abs(px.x - (x0 - frameW)) < lip || abs(px.x - (x0 + paneW - mul * 0.5)) < lip)) shade = 1.28;
        if (frame && (abs(px.x - (x1 + frameW)) < lip || abs(px.x - (x0 + paneW + mul * 0.5)) < lip || abs(px.y - (y0 - frameW)) < lip)) shade = 0.62;
        if (frame) col = wood * shade + lamp * lampA * (0.55 + 0.9 * left);
        if (sill) {
            float3 body = float3(0.026, 0.012, 0.006) * (0.85 + 0.35 * left) + lamp * lampA * (0.8 + left);
            bool topFace = px.y <= y1 + 18.0 * s;
            col = topFace ? (float3(0.14, 0.06, 0.028) * (0.9 + 0.4 * left) + lamp * (0.06 + 0.12 * left)) : body;
        }
        float rel = saturate((px.y - y0) / max(oh, 1.0));
        float3 outC = lerp(pow(float3(26.0, 39.0, 64.0) / 255.0, 2.2) * 2.8, pow(float3(60.0, 86.0, 120.0) / 255.0, 2.2) * 3.8, pow(rel, 0.75));
        float frac = (y0 + 0.76 * oh) / height;
        if (glass && plates != 0) {
            outC = plateStack(i.uv);
        } else if (glass) {
            float trees = pineCard(px, frac, 14.0, 300.0, 78.0, 21.0, 0.035, 0.08, 2.0, 0.0, 0.0);
            outC = lerp(outC, pow(float3(21.0, 32.0, 48.0) / 255.0, 2.2) * 2.4, saturate(trees) * 0.82);
            float g = groundPx(px.x, frac, 14.0, 300.0, 21.2);
            float feather = max(36.0 * s, 4.0);
            float drift = smoothstep(g - feather, g + feather * 0.35, px.y);
            float3 snowA = pow(float3(62.0, 86.0, 116.0) / 255.0, 2.2) * 3.2;
            float3 snowB = pow(float3(90.0, 116.0, 148.0) / 255.0, 2.2) * 3.5;
            outC = lerp(outC, lerp(snowA, snowB, saturate((px.y - g) / max(0.26 * oh, 1.0))), drift);
        }
        [unroll] for (int li = 0; li < 3; li++) {
            float fx = li == 0 ? 0.30 : (li == 1 ? 0.58 : 0.80);
            float fy = li == 0 ? 0.70 : (li == 1 ? 0.66 : 0.72);
            float rad = li == 1 ? 8.0 : (li == 2 ? 12.0 : 10.0);
            float2 lp = float2(x0, y0) + float2(fx, fy) * float2(ow, oh);
            float dist = length(px - lp);
            float disc = smoothstep((rad + 5.0) * s, max((rad - 1.0) * s, 0.5), dist);
            float core = exp(-dist * dist / max(pow(2.4 * s, 2.0), 0.8));
            outC = lerp(outC, float3(1.0, 0.48, 0.16), disc * 0.42);
            outC += float3(1.0, 0.72, 0.45) * core * 0.28;
        }
        float3 tint = flake * float3(0.42, 0.50, 0.64);
        float frost = 0.0;
        if (glass) {
            float lx = px.x - x0;
            float ly = px.y - y0;
            float pxn = clamp(floor(lx / paneW), 0.0, 1.0);
            float pyn = clamp(floor(ly / paneH), 0.0, 2.0);
            float ox = lx - pxn * paneW;
            float oy = ly - pyn * paneH;
            float amt = smoothstep(0.12, 0.88, hash12(float2(pxn + pyn * 2.0 + 1.0, 8.0)));
            float paneSize = max(min(paneW, paneH), 1.0);
            float lower = min(length(float2(ox, paneH - oy)), length(float2(paneW - ox, paneH - oy))) / paneSize;
            float upper = min(length(float2(ox, oy)), length(float2(paneW - ox, oy))) / paneSize;
            frost = smoothstep(0.62, 0.02, lower) * lerp(0.25, 1.0, amt)
                  + smoothstep(0.26, 0.02, upper) * 0.30 * amt
                  + smoothstep(0.30, 0.0, (paneH - oy) / max(paneH, 1.0)) * lerp(0.04, 0.42, amt);
            float fernU = (ox * 0.20 + oy) / max(18.0 * s, 1.0);
            float fernV = (min(ox, paneW - ox) - min(oy, paneH - oy)) / max(7.0 * s, 1.0);
            float fern = vnoise(float2(fernU, fernV + pxn));
            float ridge = 1.0 - abs(2.0 * fern - 1.0);
            frost = saturate(frost * pow(saturate(ridge), 1.15) * lerp(0.20, 1.0, amt));
            frostM = frost;
            float3 frostC = (px.x < x0 + paneW && px.y > y0 + 1.2 * paneH)
                ? float3(0.72, 0.80, 0.88) : float3(0.68, 0.78, 0.90);
            outC = lerp(outC, frostC, saturate(frost * 0.62));
        }
        float n = vnoise(float2(px.x / max(80.0 * s, 1.0), 2.2));
        float heightS = (20.0 + 12.0 * n) * s;
        float st = (px.y - (y1 + s)) / max(heightS, 1.0);
        float prof = sqrt(saturate(1.0 - pow(saturate(st), 2.0))) * smoothstep(1.05, 0.55, st);
        if (px.x >= x0 + 8.0 * s && px.x <= x1 - 8.0 * s && px.y > y1 && st >= 0.0 && st <= 1.05) {
            sillM = prof;
        }
        col *= 4.2;
        if (glass) col = outC;
        if (sillM > 0.02) col = lerp(col, float3(0.78, 0.86, 0.94) * 1.15 + lamp * 0.05 * left, sillM);
        // outdoor snow, masked to the glass, composited after the room lift
        if (glass) {
            float3 snowC = col;
            snowDisc(snowC, px, 52.0, 44.0, 1.4, 2.2, 0.95, 0.40, 0.55, 32.0, 4.5, 0.70, 1.0, 900.0, 1.0, tint, 0.4);
            snowDisc(snowC, px, 96.0, 84.0, 3.2, 5.2, 1.3, 0.62, 0.82, 64.0, 12.0, 0.50, 1.0, 340.0, 2.0, tint, 0.4);
            snowDisc(snowC, px, 180.0, 160.0, 6.0, 9.0, 1.7, 0.75, 0.90, 112.0, 22.0, 0.35, 1.28, 80.0, 3.0, tint, 0.4);
            col = snowC;
        }
        if (p3 < -0.5) return float4(frameM, glassM, frostM, sillM);
        return float4(toLin(tonemap(col)), 1.0);
    }
    float3 tint = flake * ambient;
    if (backdrop < 0.5 || quality > 0.5) {
        snowDisc(col, px, 52.0, 44.0, 1.4, 2.2, 0.55, 0.40, 0.55, 32.0, 4.5, 0.70, 1.0, 900.0, 1.0, tint, 0.0);
    }
    snowDisc(col, px, 96.0, 84.0, 3.2, 5.2, 0.9, 0.62, 0.82, 64.0, 12.0, 0.50, 1.0, 340.0, 2.0, tint, 0.0);
    snowDisc(col, px, 180.0, 160.0, 6.0, 9.0, 1.3, 0.75, 0.90, 112.0, 22.0, 0.35, 1.28, 80.0, 3.0, tint, 0.0);
    snowDisc(col, px, 520.0, 480.0, 18.0, 30.0, 4.0, 0.10, 0.18, 200.0, 36.0, 0.25, 1.25, 5.0, 4.0, tint, 0.0);
    if (backdrop >= 0.5 && backdrop < 1.5) col *= 1.0;
    if (p3 < -0.5) return float4(farC, midC, nearC, 0.0);
    return float4(toLin(tonemap(col)), 1.0);
}

float4 PSFallback(VSOut i) : SV_Target {
    float3 c = lerp(float3(0.02, 0.025, 0.04), float3(0.045, 0.05, 0.07), i.uv.y);
    return float4(pow(c, 2.2), 1.0);
}
)";

static const char kLavaHLSL[] = R"(
cbuffer Constants : register(b0) {
    float time;
    float width;
    float height;
    float speed;
    float4 lavaColor;
    float4 liquidColor;
    float glow;
    float blobScale;
    float blobCount;
    float threshold;
    float heat;
    float gradient;
    float lamp;
    float specGain;
    float lookX;
    float lookY;
    float pad0;
    float pad1;
    float4 blobs[16];
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 toLin(float3 c) { return pow(saturate(c), 2.2); }

float4 PSLava(VSOut i) : SV_Target {
    float2 uv = i.uv + float2(lookX, -lookY) * 0.35;
    float aspect = width / max(height, 1.0);
    int n = (int)blobCount;
    float k = 0.0;
    float tsum = 0.0;
    float wsum = 0.0;
    float under = 0.0;
    float2 grad = float2(0.0, 0.0);
    [loop] for (int bi = 0; bi < 16; bi++) {
        if (bi >= n) break;
        float rad = blobs[bi].z * blobScale;
        if (rad < 0.008) continue;
        float2 p = blobs[bi].xy;
        float2 d = uv - p;
        d.x *= aspect;
        float q = dot(d, d) / (rad * rad);
        float s = saturate(1.0 - q);
        float f = s * s * s;
        k += f;
        tsum += blobs[bi].w * f;
        wsum += f;
        under += saturate(d.y / rad) * f;
        // f = s^3, s = max(0, 1-q). df/dd = -6 s^2 d / r^2, which is 0 at the
        // crown and grows smoothly toward the edge. Do not normalize this.
        if (s > 0.0) grad += ((-6.0 * s * s) / max(rad * rad, 1e-4)) * d;
    }
    float temp = (wsum > 1e-5) ? saturate(tsum / wsum) : 0.0;
    float u = (wsum > 1e-5) ? saturate(under / wsum) : 0.0;
    temp = saturate(temp * lerp(0.72, 1.28, heat) + (heat - 0.5) * 0.06);

    float iso = max(threshold, 0.26);
    // Width of one pixel in field units, so the edge stays about a pixel wide
    // for both small and large blobs.
    float fw = max(length(grad), 1.0) / max(height, 1.0);
    float body = smoothstep(iso - fw, iso + fw, k);

    float bloom = saturate((k - iso * 0.20) / max(iso * 0.95, 1e-3));
    bloom = bloom * bloom * (1.0 - body);

    float3 hot = saturate(lavaColor.rgb * float3(1.18, 1.10, 0.28) + float3(0.20, 0.42, 0.05));
    float3 cool = saturate(lavaColor.rgb * float3(0.82, 0.18, 0.70) + float3(0.30, 0.08, 0.36));
    float hotLum = max(dot(hot, float3(0.30, 0.59, 0.11)), 1e-3);
    float coolLum = max(dot(cool, float3(0.30, 0.59, 0.11)), 1e-3);
    cool *= (hotLum * 0.60) / coolLum;
    cool = saturate(cool);
    float e0 = lerp(0.30, 0.06, gradient);
    float e1 = lerp(0.92, 0.40, gradient);
    float3 wax = lerp(cool, hot, smoothstep(e0, e1, temp));

    // Height-field normal. Keeping the gradient length makes a soft dome.
    // Dividing by length(grad) first (1.9.0) forced a constant tilt — a cone
    // with a hard disc where the gradient dropped out, and a tight specular
    // that broke into facets and grain. 0.022 matches lavaSurfaceContinuity.
    float2 slope = grad * 0.022;
    float3 nrm = normalize(float3(-slope.x, -slope.y, 1.0));
    float3 light = normalize(float3(-0.32, -0.48, 0.82));
    float ndl = saturate(dot(nrm, light));
    float3 halfv = normalize(light + float3(0.0, 0.0, 1.0));
    float spec = pow(saturate(dot(nrm, halfv)), 8.0) * body;

    float core = saturate((k - iso) * 2.0);
    float lift = lerp(0.92, 1.20, u);
    float3 lit = wax * lerp(0.96, 1.12, core) * lift;
    lit *= lerp(0.96, 1.05, ndl);
    lit = max(lit, wax * 0.90);
    lit += float3(1.0, 0.97, 0.92) * spec * (0.12 + 0.06 * specGain);
    float rim = (1.0 - core) * body * 0.22;
    lit += float3(1.00, 0.88, 0.48) * rim;

    float bottom = uv.y;
    float band = pow(saturate(bottom), 11.0);
    float fill = pow(saturate(bottom), 4.2) * 0.30;
    float2 lp = float2((uv.x - 0.5) * aspect * 0.58, bottom - 1.03);
    float radial = exp(-dot(lp, lp) * 13.5);
    float heatGlow = (radial * 1.40 + band * 1.20 + fill) * glow;
    float3 heatCol = lavaColor.rgb * float3(1.32, 0.82, 0.10) + float3(0.42, 0.16, 0.0);
    float3 heater = heatCol * heatGlow;
    heater = heater / (1.0 + heater * 0.28);

    float3 liquid = liquidColor.rgb * 0.22 + heater * 0.92;
    liquid = saturate(liquid);

    float3 col = lerp(liquid, lit, body);
    col += wax * bloom * (0.18 + 0.42 * glow);
    col += heater * 0.10 * (1.0 - body);

    float2 g = float2((uv.x - 0.5) * aspect * 1.15, (uv.y - 0.46) * 1.02);
    float capsule = length(g);
    float glass = smoothstep(0.50, 0.40, capsule);
    float edge = smoothstep(0.50, 0.42, capsule) * smoothstep(0.30, 0.40, capsule);
    float3 lampCol = col * glass + float3(0.75, 0.88, 1.0) * edge * 0.35;
    float3 outside = liquidColor.rgb * 0.05;
    col = lerp(col, lerp(outside, lampCol, glass), saturate(lamp));
    return float4(toLin(saturate(col)), 1.0);
}
)";

static const char kGradeHLSL[] = R"(
cbuffer Constants : register(b0) {
    float brightness;
    float saturation;
    float vignette;
    float night;
    float width;
    float height;
    float pad0;
    float pad1;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

Texture2D sceneTex : register(t0);
SamplerState sceneSamp : register(s0);

float ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float4 PSGrade(VSOut i) : SV_Target {
    float3 lin = sceneTex.Sample(sceneSamp, i.uv).rgb;
    // One soft disc of bright pixels. Cheap stand-in for a Kawase pyramid: six taps, highlights only.
    float3 bloom = 0.0;
    float2 texel = float2(1.0 / max(width, 1.0), 1.0 / max(height, 1.0)) * 16.0;
    [unroll] for (int k = 0; k < 6; k++) {
        float ang = (float)k * 1.04719755;
        float3 s = sceneTex.Sample(sceneSamp, i.uv + float2(cos(ang), sin(ang)) * texel).rgb;
        float m = max(s.r, max(s.g, s.b));
        bloom += s * saturate((m - 0.45) / 0.50);
    }
    lin += bloom * (0.055 / 6.0);
    float l = dot(lin, float3(0.2126, 0.7152, 0.0722));
    lin = lerp(l.xxx, lin, saturation);
    lin *= brightness;
    lin = lerp(lin, lin * float3(0.62, 0.72, 0.95) * 0.78, saturate(night));
    float2 p = (i.uv - 0.5) * float2(width / max(height, 1.0), 1.0);
    float vig = smoothstep(1.05, 0.22, length(p));
    lin *= lerp(1.0, vig, saturate(vignette));
    float3 enc = pow(max(lin, 0.0), 1.0 / 2.2);
    enc += (ign(i.pos.xy) - 0.5) * (1.0 / 255.0);
    return float4(saturate(enc), 1.0);
}
)";
