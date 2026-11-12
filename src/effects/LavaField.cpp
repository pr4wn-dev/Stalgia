#include "effects/LavaField.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {

constexpr double kSlope = 0.022;  // must match PSLava `grad * 0.022`

struct Blob {
    double x, y, r;
};

double kernel(double px, double py, const Blob& b, double& gx, double& gy) {
    const double dx = px - b.x;
    const double dy = py - b.y;
    const double rad2 = b.r * b.r;
    const double q = (dx * dx + dy * dy) / rad2;
    double s = 1.0 - q;
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    if (s > 0.0) {
        const double scale = -6.0 * s * s / rad2;
        gx += scale * dx;
        gy += scale * dy;
    }
    return s * s * s;
}

void sample(double px, double py, const Blob* blobs, int n, double& field, double& gx, double& gy) {
    field = 0;
    gx = 0;
    gy = 0;
    for (int i = 0; i < n; ++i) field += kernel(px, py, blobs[i], gx, gy);
}

double tiltAt(double gx, double gy) {
    const double sx = -gx * kSlope;
    const double sy = -gy * kSlope;
    const double len = std::sqrt(sx * sx + sy * sy + 1.0);
    const double nz = 1.0 / len;
    const double nxy = std::sqrt(sx * sx + sy * sy) / len;
    return nxy / std::max(nz, 1e-8);
}

}  // namespace

bool lavaSurfaceContinuity(std::string& why) {
    const Blob blobs[] = {{0.50, 0.55, 0.12}, {0.62, 0.48, 0.07}};
    const int n = 2;
    double worst = 0;
    const double h = 1e-4;
    for (int iy = 0; iy < 9; ++iy) {
        for (int ix = 0; ix < 9; ++ix) {
            const double px = 0.42 + ix * 0.02;
            const double py = 0.46 + iy * 0.02;
            double f, gx, gy;
            sample(px, py, blobs, n, f, gx, gy);
            double fp, fm, gp, gm;
            double dump;
            sample(px + h, py, blobs, n, fp, dump, dump);
            sample(px - h, py, blobs, n, fm, dump, dump);
            sample(px, py + h, blobs, n, gp, dump, dump);
            sample(px, py - h, blobs, n, gm, dump, dump);
            const double ngx = (fp - fm) / (2.0 * h);
            const double ngy = (gp - gm) / (2.0 * h);
            const double mag = std::max(std::hypot(gx, gy), 1.0);
            worst = std::max(worst, std::hypot(ngx - gx, ngy - gy) / mag);
        }
    }
    if (worst > 0.02) {
        std::ostringstream o;
        o << "gradient error " << worst;
        why = o.str();
        return false;
    }

    const Blob one[] = {{0.50, 0.50, 0.12}};
    // s^3 is flat at the crown and again at the kernel edge, steepest in
    // between. A cone (normalized gradient) is flat everywhere past the crown.
    constexpr int kTiltN = 12;
    double tilts[kTiltN];
    double maxTilt = 0;
    double maxJump = 0;
    for (int i = 0; i < kTiltN; ++i) {
        const double frac = (0.90 * i) / (kTiltN - 1);
        double f, gx, gy;
        sample(0.50 + frac * 0.12, 0.50, one, 1, f, gx, gy);
        tilts[i] = tiltAt(gx, gy);
        maxTilt = std::max(maxTilt, tilts[i]);
        if (i) maxJump = std::max(maxJump, std::fabs(tilts[i] - tilts[i - 1]));
    }
    if (tilts[0] > 0.02) {
        why = "crown is not flat";
        return false;
    }
    if (maxTilt < 0.15) {
        why = "surface has no roll-off";
        return false;
    }
    if (!(tilts[1] > tilts[0] + 0.02)) {
        why = "crown does not roll off";
        return false;
    }
    if (maxJump > 0.20) {
        std::ostringstream o;
        o << "tilt jump " << maxJump;
        why = o.str();
        return false;
    }
    int flat = 0;
    for (int i = 1; i < kTiltN; ++i)
        if (std::fabs(tilts[i] - tilts[i - 1]) < 0.01) ++flat;
    if (flat > kTiltN - 3) {
        why = "tilt is a cone";
        return false;
    }

    double prevShade = 0;
    double prevStep = 0;
    bool have = false;
    double maxStep = 0;
    const int steps = 72;
    for (int i = 0; i <= steps; ++i) {
        const double px = 0.50 + (0.10 * i) / steps;
        double f, gx, gy;
        sample(px, 0.50, one, 1, f, gx, gy);
        const double sx = -gx * kSlope;
        const double sy = -gy * kSlope;
        const double inv = 1.0 / std::sqrt(sx * sx + sy * sy + 1.0);
        const double ndl = std::max(0.0, (-0.32 * sx + -0.48 * sy + 0.82) * inv /
                                             std::sqrt(0.32 * 0.32 + 0.48 * 0.48 + 0.82 * 0.82));
        const double spec = std::pow(ndl, 8.0);
        const double shade = 0.45 + 0.40 * ndl + 0.12 * spec;
        const double enc = std::pow(std::min(1.0, std::max(0.0, shade)), 1.0 / 2.2);
        if (have) {
            const double step = std::fabs(enc - prevShade);
            maxStep = std::max(maxStep, step);
            if (i > 1 && step > prevStep * 4.0 + (3.0 / 255.0)) {
                std::ostringstream o;
                o << "shade step " << step;
                why = o.str();
                return false;
            }
            prevStep = step;
        }
        prevShade = enc;
        have = true;
    }
    if (maxStep > 6.0 / 255.0) {
        std::ostringstream o;
        o << "banding step " << maxStep;
        why = o.str();
        return false;
    }

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "grad=" << worst << " tilt=" << tilts[0] << "," << tilts[3] << " step=" << (maxStep * 255.0)
      << "lsb";
    why = o.str();
    return true;
}
