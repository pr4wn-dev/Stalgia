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

float3 tonemap(float3 c) { return 1.0 - exp(-1.1 * max(c, 0.0)); }
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
    float tw = 0.90 + 0.10 * sin(time * (0.45 + n * 1.4) + n * 42.0);
    float amp = lerp(1.0, tw, twinkle);
    float gain = sky ? (0.34 + 2.15 * bright) : (0.35 + 1.15 * bright);
    float inten = (core * gain + halo) * amp * density;
    col += bb * inten * (0.55 + 0.45 * depth) * tint;
}

void starLayer(inout float3 col, float2 p, float scale, float depth, float keep, float twinkle,
               float density, float3 tint, float field) {
    float2 q = p * scale;
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
        float base = lerp(0.92, 0.64, heightK) - lift;
        base += (fold - 0.48) * 0.42;
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
    float i3 = smoothstep(-0.01, 0.004, a3) * exp(-max(a3, 0.0) / 0.16) * 0.06;
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
    float skyT = smoothstep(0.0, 0.9, uv.y);
    float3 col = lerp(float3(0.004, 0.006, 0.018), float3(0.009, 0.013, 0.030), skyT);
    col += float3(0.005, 0.009, 0.016) * smoothstep(0.55, 1.0, uv.y);

    float diag = p.y * 0.95 + p.x * 0.78;
    float along = p.x * 0.95 - p.y * 0.78;
    float bandScale = max(1.0 - travel * 0.55, 0.42);
    // Wide gaussian, evaluated on every pixel. The old cheap>0.003 test cut this
    // glow on two straight diagonals while it was still bright.
    float env = exp(-(diag * diag) / (2.0 * 0.30 * 0.30));
    float coreA = exp(-((along - 0.15) * (along - 0.15)) / 0.28);
    float3 glowCol = lerp(float3(0.55, 0.66, 0.98), float3(0.92, 0.68, 0.40), coreA);
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
        glow *= ext;
        glow += detail * fade;
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
            float len = lerp(0.15, 0.85, travel);
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
    if (nebula > 0.08 && quality > 0.5) {
        float blob = exp(-(p.x + 0.05) * (p.x + 0.05) / 0.70 - (p.y + 0.02) * (p.y + 0.02) / 0.28);
        float blob2 = exp(-(p.x - 0.45) * (p.x - 0.45) / 0.35 - (p.y - 0.12) * (p.y - 0.12) / 0.16);
        float n1 = fbm2(p * 1.15 + float2(time * 0.008, -0.4));
        float n2 = fbm2(float2(p.x * 2.4 + 1.7, p.y * 1.8 + 0.6));
        float veil = smoothstep(0.36, 0.78, n1) * (0.40 + 0.60 * n2) * (blob + 0.65 * blob2) * nebula;
        float3 nebCol = lerp(float3(0.16, 0.07, 0.24), tint, 0.62);
        col += nebCol * veil * 1.85;
    }

    if (travel < 0.45) {
        float period = 9.0;
        float cycle = floor(time / period);
        float u = frac(time / period);
        if (u < 0.16) {
            float2 start = float2(hash12(float2(cycle, 1.7)) * 1.3 - 0.75, hash12(float2(cycle, 4.2)) * 0.50 - 0.38);
            float2 dir = normalize(float2(0.86, 0.18 + 0.22 * hash12(float2(cycle, 8.0))));
            float2 head = start + dir * (u / 0.16) * 0.72;
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
            float life = u / 0.16;
            float fade = sin(pow(life, 0.55) * 3.14159265);
            col += headG * fade * float3(1.15, 1.05, 0.90) + trail * fade * float3(1.05, 0.72, 0.42);
        }
    }
    return float4(toLin(tonemap(col)), 1.0);
}

float4 PSAurora(VSOut i) : SV_Target {
    float2 uv = i.uv + float2(lookX, -lookY) * 0.12;
    float aspect = width / max(height, 1.0);
    float2 pxy = float2(uv.x * aspect, uv.y);
    float t = time * speed;
    float skyT = smoothstep(0.0, 0.88, uv.y);
    float3 col = lerp(float3(0.002, 0.003, 0.008), float3(0.008, 0.010, 0.020), skyT);
    if (p2 > 0.5) {
        float2 sp = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
        starLayer(col, sp, 80.0, 0.7, 0.012, 0.35, 0.55, float3(0.85, 0.90, 1.0), 0.0);
    }
    float surge = 0.72 + 0.28 * vnoise(float2(t * 0.045, 2.2));
    float lift = (p3 > 0.5) ? 0.10 : 0.0;
    col += auroraRibbons(pxy.x, pxy.y, t, p0, p1, colorA.rgb, colorB.rgb, surge, aspect, lift, 1.0);
    if (p3 > 0.5 && quality > 0.5) {
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
