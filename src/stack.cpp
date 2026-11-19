// SPDX-License-Identifier: GPL-2.0-or-later
#include "stack.h"

#include "he_decode.h"
#include "meta.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace nev {

namespace {

struct Img {
    int w = 0, h = 0;
    std::vector<float> d;
    Img() = default;
    Img(int w_, int h_, float v = 0) : w(w_), h(h_), d(size_t(w_) * h_, v) {}
    float &at(int x, int y) { return d[size_t(y) * w + x]; }
    float at(int x, int y) const { return d[size_t(y) * w + x]; }
};

unsigned g_threads = 1;

// Run fn(begin, end) over [0, n) in parallel chunks.
template <class F> void parallel_for(int n, F fn) {
    unsigned nt = std::min<unsigned>(g_threads, unsigned(std::max(n, 1)));
    if (nt <= 1) { fn(0, n); return; }
    std::atomic<int> next{0};
    const int chunk = std::max(1, n / int(nt * 8));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++)
        th.emplace_back([&]() {
            for (int b; (b = next.fetch_add(chunk)) < n;) fn(b, std::min(n, b + chunk));
        });
    for (auto &t : th) t.join();
}

// A decoded frame split into its four CFA planes (black-subtracted, exposure-normalised).
struct Planes {
    int w = 0, h = 0;          // plane size (half the sensor size)
    Img p[4];                  // CFA sites (0,0) (1,0) (0,1) (1,1)
    std::vector<uint8_t> clip; // per quad: any sample at/above the clip level
};

Planes split(const std::vector<uint16_t> &raw, unsigned W, unsigned H, float black, float scale, unsigned clip_level) {
    Planes P;
    P.w = int(W / 2); P.h = int(H / 2);
    for (auto &i : P.p) i = Img(P.w, P.h);
    P.clip.assign(size_t(P.w) * P.h, 0);
    parallel_for(P.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint16_t *r0 = raw.data() + size_t(2 * y) * W, *r1 = r0 + W;
            for (int x = 0; x < P.w; x++) {
                uint16_t v[4] = {r0[2 * x], r0[2 * x + 1], r1[2 * x], r1[2 * x + 1]};
                bool c = false;
                for (int k = 0; k < 4; k++) {
                    P.p[k].at(x, y) = (float(v[k]) - black) * scale;
                    c |= v[k] >= clip_level;
                }
                P.clip[size_t(y) * P.w + x] = c;
            }
        }
    });
    return P;
}

Img grey(const Planes &P) {
    Img g(P.w, P.h);
    parallel_for(P.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < P.w; x++)
                g.at(x, y) = 0.25f * (P.p[0].at(x, y) + P.p[1].at(x, y) + P.p[2].at(x, y) + P.p[3].at(x, y));
    });
    return g;
}

Img down2(const Img &s) {
    Img d(s.w / 2, s.h / 2);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++)
            d.at(x, y) = 0.25f * (s.at(2 * x, 2 * y) + s.at(2 * x + 1, 2 * y) + s.at(2 * x, 2 * y + 1) + s.at(2 * x + 1, 2 * y + 1));
    return d;
}

std::vector<Img> pyramid(const Img &g) {
    std::vector<Img> v{g};
    while (v.size() < 4 && v.back().w >= 128 && v.back().h >= 128) v.push_back(down2(v.back()));
    return v;
}

Img box3(const Img &s) {
    Img t(s.w, s.h), d(s.w, s.h);
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
            int a = std::max(x - 1, 0), b = std::min(x + 1, s.w - 1);
            t.at(x, y) = (s.at(a, y) + s.at(x, y) + s.at(b, y)) / 3;
        }
    for (int y = 0; y < s.h; y++) {
        int a = std::max(y - 1, 0), b = std::min(y + 1, s.h - 1);
        for (int x = 0; x < s.w; x++) d.at(x, y) = (t.at(x, a) + t.at(x, y) + t.at(x, b)) / 3;
    }
    return d;
}

// Displacement field on a tile grid: ref(p) ~ frame(p + d), in plane pixels.
struct Field {
    int tx = 0, ty = 0, T = 32;
    std::vector<float> dx, dy;
    void bilinear(float px, float py, float &ox, float &oy) const {
        float fx = std::clamp((px - T * 0.5f) / T, 0.0f, float(tx - 1));
        float fy = std::clamp((py - T * 0.5f) / T, 0.0f, float(ty - 1));
        int x0 = std::min(int(fx), tx - 2 < 0 ? 0 : tx - 2), y0 = std::min(int(fy), ty - 2 < 0 ? 0 : ty - 2);
        int x1 = std::min(x0 + 1, tx - 1), y1 = std::min(y0 + 1, ty - 1);
        float ax = fx - x0, ay = fy - y0;
        auto lerp = [&](const std::vector<float> &v) {
            float a = v[size_t(y0) * tx + x0] * (1 - ax) + v[size_t(y0) * tx + x1] * ax;
            float b = v[size_t(y1) * tx + x0] * (1 - ax) + v[size_t(y1) * tx + x1] * ax;
            return a * (1 - ay) + b * ay;
        };
        ox = lerp(dx); oy = lerp(dy);
    }
};

void median3(Field &F) {
    auto filt = [&](std::vector<float> &v) {
        std::vector<float> o(v.size());
        for (int y = 0; y < F.ty; y++)
            for (int x = 0; x < F.tx; x++) {
                float n[9]; int k = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int xx = std::clamp(x + i, 0, F.tx - 1), yy = std::clamp(y + j, 0, F.ty - 1);
                        n[k++] = v[size_t(yy) * F.tx + xx];
                    }
                std::nth_element(n, n + 4, n + 9);
                o[size_t(y) * F.tx + x] = n[4];
            }
        v.swap(o);
    };
    filt(F.dx); filt(F.dy);
}

// Mean squared difference between ref window centred at (cx, cy) and frame shifted by (ox, oy).
double msd(const Img &r, const Img &f, int cx, int cy, int hw, int ox, int oy) {
    int x0 = std::max({cx - hw, 0, -ox}), x1 = std::min({cx + hw, r.w, f.w - ox});
    int y0 = std::max({cy - hw, 0, -oy}), y1 = std::min({cy + hw, r.h, f.h - oy});
    if (x1 - x0 < hw || y1 - y0 < hw) return INFINITY;  // less than a quarter of the window overlaps
    double s = 0;
    for (int y = y0; y < y1; y++) {
        const float *a = &r.d[size_t(y) * r.w], *b = &f.d[size_t(y + oy) * f.w + ox];
        for (int x = x0; x < x1; x++) { float e = a[x] - b[x]; s += double(e) * e; }
    }
    return s / (double(x1 - x0) * (y1 - y0));
}

// Whole-image translation (exhaustive search over +-rad on the central 80 %).
void global_shift(const Img &r, const Img &f, int rad, int &gx, int &gy) {
    const int x0 = r.w / 10, x1 = r.w - r.w / 10, y0 = r.h / 10, y1 = r.h - r.h / 10;
    const int n = 2 * rad + 1;
    std::vector<double> e(size_t(n) * n, INFINITY);
    parallel_for(n * n, [&](int a, int b) {
        for (int k = a; k < b; k++) {
            int ox = k % n - rad, oy = k / n - rad;
            int xa = std::max(x0, -ox), xb = std::min(x1, f.w - ox), ya = std::max(y0, -oy), yb = std::min(y1, f.h - oy);
            if (xb - xa < (x1 - x0) / 2 || yb - ya < (y1 - y0) / 2) continue;
            double s = 0;
            for (int y = ya; y < yb; y++) {
                const float *p = &r.d[size_t(y) * r.w], *q = &f.d[size_t(y + oy) * f.w + ox];
                for (int x = xa; x < xb; x++) { float d = p[x] - q[x]; s += double(d) * d; }
            }
            e[size_t(k)] = s / (double(xb - xa) * (yb - ya));
        }
    });
    size_t best = size_t(std::min_element(e.begin(), e.end()) - e.begin());
    gx = int(best % size_t(n)) - rad; gy = int(best / size_t(n)) - rad;
}

// Replace tiles that disagree with their 5x5 neighbourhood median by more than thr.
void reject_outliers(Field &F, float thr) {
    std::vector<float> ox = F.dx, oy = F.dy;
    for (int y = 0; y < F.ty; y++)
        for (int x = 0; x < F.tx; x++) {
            float nx[25], ny[25]; int k = 0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= F.tx || yy >= F.ty) continue;
                    nx[k] = F.dx[size_t(yy) * F.tx + xx]; ny[k] = F.dy[size_t(yy) * F.tx + xx]; k++;
                }
            std::nth_element(nx, nx + k / 2, nx + k); std::nth_element(ny, ny + k / 2, ny + k);
            size_t o = size_t(y) * F.tx + x;
            if (std::hypot(F.dx[o] - nx[k / 2], F.dy[o] - ny[k / 2]) > thr) { ox[o] = nx[k / 2]; oy[o] = ny[k / 2]; }
        }
    F.dx.swap(ox); F.dy.swap(oy);
}

// Camera motion as an affine field (translation, rotation, scale, shear: also covers most
// rolling-shutter skew), fitted robustly (IRLS, Tukey) to the tile vectors. Tiles further
// than max_dev from it (moving objects, mismatches) are replaced by the model.
void clamp_to_affine(Field &F, float max_dev) {
    const size_t n = F.dx.size();
    std::vector<double> X(n), Y(n), w(n, 1.0);
    for (size_t i = 0; i < n; i++) {
        X[i] = ((double(i % size_t(F.tx)) + 0.5) / F.tx - 0.5) * 2;
        Y[i] = ((double(i / size_t(F.tx)) + 0.5) / F.ty - 0.5) * 2;
    }
    double a[3] = {}, b[3] = {};
    auto solve = [&]() {
        double M[3][3] = {}, vx[3] = {}, vy[3] = {};
        for (size_t i = 0; i < n; i++) {
            double r[3] = {1, X[i], Y[i]};
            for (int p = 0; p < 3; p++) {
                for (int q = 0; q < 3; q++) M[p][q] += w[i] * r[p] * r[q];
                vx[p] += w[i] * r[p] * F.dx[i]; vy[p] += w[i] * r[p] * F.dy[i];
            }
        }
        // Gaussian elimination (3x3, two right-hand sides)
        for (int c = 0; c < 3; c++) {
            int piv = c;
            for (int r = c + 1; r < 3; r++) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
            std::swap(M[c], M[piv]); std::swap(vx[c], vx[piv]); std::swap(vy[c], vy[piv]);
            if (std::fabs(M[c][c]) < 1e-12) return false;
            for (int r = 0; r < 3; r++) {
                if (r == c) continue;
                double f = M[r][c] / M[c][c];
                for (int q = 0; q < 3; q++) M[r][q] -= f * M[c][q];
                vx[r] -= f * vx[c]; vy[r] -= f * vy[c];
            }
        }
        for (int c = 0; c < 3; c++) { a[c] = vx[c] / M[c][c]; b[c] = vy[c] / M[c][c]; }
        return true;
    };
    const double c = std::max(2.0 * max_dev, 1.0);  // Tukey cut-off
    for (int it = 0; it < 8; it++) {
        if (!solve()) return;
        for (size_t i = 0; i < n; i++) {
            double r = std::hypot(F.dx[i] - (a[0] + a[1] * X[i] + a[2] * Y[i]), F.dy[i] - (b[0] + b[1] * X[i] + b[2] * Y[i]));
            double u = r / c;
            w[i] = u < 1 ? (1 - u * u) * (1 - u * u) : 0;
        }
    }
    for (size_t i = 0; i < n; i++) {
        double mx = a[0] + a[1] * X[i] + a[2] * Y[i], my = b[0] + b[1] * X[i] + b[2] * Y[i];
        if (std::hypot(F.dx[i] - mx, F.dy[i] - my) > max_dev) { F.dx[i] = float(mx); F.dy[i] = float(my); }
    }
}

// Coarse-to-fine tile alignment, regularised against moving objects: a global translation
// seeds the coarsest level, each level searches only +-2 around its prior, and after every
// level outliers go to their neighbourhood median and to the camera's affine motion.
Field align(const std::vector<Img> &R, const std::vector<Img> &F, int T) {
    Field fd;
    fd.T = T;
    fd.tx = (R[0].w + T - 1) / T; fd.ty = (R[0].h + T - 1) / T;
    fd.dx.assign(size_t(fd.tx) * fd.ty, 0); fd.dy.assign(fd.dx.size(), 0);
    const int L = int(std::min(R.size(), F.size())) - 1;
    {
        int gx, gy;
        global_shift(R[size_t(L)], F[size_t(L)], 16, gx, gy);
        std::fill(fd.dx.begin(), fd.dx.end(), float(gx << L));
        std::fill(fd.dy.begin(), fd.dy.end(), float(gy << L));
    }
    for (int l = L; l >= 0; l--) {
        const int s = 1 << l;
        const int radius = 2;
        const int hw = std::max(T / 2 / s, 8);
        std::vector<float> nx(fd.dx.size()), ny(fd.dy.size());
        parallel_for(fd.ty, [&](int t0, int t1) {
            for (int ty = t0; ty < t1; ty++)
                for (int tx = 0; tx < fd.tx; tx++) {
                    size_t i = size_t(ty) * fd.tx + tx;
                    int cx = (tx * T + T / 2) / s, cy = (ty * T + T / 2) / s;
                    int px = int(std::lround(fd.dx[i] / s)), py = int(std::lround(fd.dy[i] / s));
                    double best = INFINITY; int bx = px, by = py;
                    for (int j = -radius; j <= radius; j++)
                        for (int k = -radius; k <= radius; k++) {
                            double e = msd(R[size_t(l)], F[size_t(l)], cx, cy, hw, px + k, py + j);
                            // tie-break towards the prior (flat tiles)
                            e *= 1.0 + 1e-3 * (std::abs(j) + std::abs(k));
                            if (e < best) { best = e; bx = px + k; by = py + j; }
                        }
                    if (l == 0 && std::isfinite(best)) {
                        // sub-pixel: parabola through the neighbours
                        auto sub = [](double m, double c, double p) {
                            double d = m - 2 * c + p;
                            return (d > 0 && std::isfinite(m) && std::isfinite(p)) ? std::clamp(0.5 * (m - p) / d, -0.5, 0.5) : 0.0;
                        };
                        double c = msd(R[0], F[0], cx, cy, hw, bx, by);
                        double sx = sub(msd(R[0], F[0], cx, cy, hw, bx - 1, by), c, msd(R[0], F[0], cx, cy, hw, bx + 1, by));
                        double sy = sub(msd(R[0], F[0], cx, cy, hw, bx, by - 1), c, msd(R[0], F[0], cx, cy, hw, bx, by + 1));
                        nx[i] = float(bx + sx); ny[i] = float(by + sy);
                    } else {
                        nx[i] = float(bx * s); ny[i] = float(by * s);
                    }
                }
        });
        fd.dx.swap(nx); fd.dy.swap(ny);
        reject_outliers(fd, float(s));                       // 1 px at this level
        clamp_to_affine(fd, std::max(1.5f, float(s)));       // plane px
        median3(fd);
    }
    return fd;
}

inline void catmull(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2 * t2 - t);
    w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

// Resample all four planes of `src` through the field into `dst`; `valid` marks quads
// whose source footprint is inside the frame and not clipped.
void warp(const Planes &src, const Field *fd, Planes &dst, std::vector<uint8_t> &valid) {
    dst.w = src.w; dst.h = src.h;
    for (auto &i : dst.p) if (i.w != src.w || i.h != src.h) i = Img(src.w, src.h);
    valid.assign(size_t(src.w) * src.h, 0);
    for (auto &i : dst.p) std::fill(i.d.begin(), i.d.end(), 0.0f);
    parallel_for(src.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < src.w; x++) {
                size_t o = size_t(y) * src.w + x;
                if (!fd) {
                    for (int k = 0; k < 4; k++) dst.p[k].d[o] = src.p[k].d[o];
                    valid[o] = !src.clip[o];
                    continue;
                }
                float dx, dy;
                fd->bilinear(float(x), float(y), dx, dy);
                float sx = x + dx, sy = y + dy;
                int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                if (ix < 1 || iy < 1 || ix + 2 >= src.w || iy + 2 >= src.h) continue;
                float wx[4], wy[4];
                catmull(sx - ix, wx); catmull(sy - iy, wy);
                bool clip = false;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        if ((wx[i] != 0 && wy[j] != 0) && src.clip[size_t(iy - 1 + j) * src.w + ix - 1 + i]) clip = true;
                if (clip) continue;
                for (int k = 0; k < 4; k++) {
                    const Img &s = src.p[k];
                    float acc = 0;
                    for (int j = 0; j < 4; j++) {
                        const float *r = &s.d[size_t(iy - 1 + j) * s.w + ix - 1];
                        acc += wy[j] * (wx[0] * r[0] + wx[1] * r[1] + wx[2] * r[2] + wx[3] * r[3]);
                    }
                    dst.p[k].d[o] = acc;
                }
                valid[o] = 1;
            }
    });
}

// Expected std of the low-passed grey difference as a function of signal: var = A*s + B.
struct NoiseModel {
    double A = 0, B = 1;
    bool ok = false;
    double sigma(double s) const { return std::sqrt(std::max(A * std::max(s, 0.0) + B, 1e-6)); }
};

NoiseModel fit_noise(const Img &ref_lp, const Img &frm_lp, const std::vector<uint8_t> &valid) {
    const int NB = 32;
    double smax = 0;
    for (size_t i = 0; i < ref_lp.d.size(); i += 7) smax = std::max(smax, double(ref_lp.d[i]));
    smax = std::max(smax, 1.0);
    std::vector<std::vector<float>> bins(NB);
    for (size_t i = 0; i < ref_lp.d.size(); i += 3) {
        if (!valid[i]) continue;
        double s = ref_lp.d[i];
        if (s <= 0) continue;
        int b = std::min(NB - 1, int(std::sqrt(s / smax) * NB));
        bins[size_t(b)].push_back(std::fabs(frm_lp.d[i] - ref_lp.d[i]));
    }
    // Per-bin robust variance (median absolute difference), then var = A*s + B by iteratively
    // reweighted least squares: bins far above the line (moving objects or misalignment that
    // dominate a brightness range) get weight 0, so they cannot inflate the model.
    struct Bin { double s, var, n; };
    std::vector<Bin> bv;
    double total = 0;
    for (int b = 0; b < NB; b++) {
        auto &v = bins[size_t(b)];
        if (v.size() < 500) continue;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        double sig = 1.4826 * v[v.size() / 2];
        bv.push_back({smax * std::pow((b + 0.5) / NB, 2), sig * sig, double(v.size())});
        total += double(v.size());
    }
    NoiseModel m;
    if (bv.size() < 2) return m;
    std::vector<double> w(bv.size());
    // start from the bins holding the darkest 70 % of the pixels (the bulk of the image)
    double cum = 0;
    for (size_t i = 0; i < bv.size(); i++) { w[i] = cum < 0.7 * total ? bv[i].n : 0; cum += bv[i].n; }
    for (int it = 0; it < 8; it++) {
        double Sw = 0, Ss = 0, Sv = 0, Sss = 0, Ssv = 0;
        for (size_t i = 0; i < bv.size(); i++) {
            Sw += w[i]; Ss += w[i] * bv[i].s; Sv += w[i] * bv[i].var;
            Sss += w[i] * bv[i].s * bv[i].s; Ssv += w[i] * bv[i].s * bv[i].var;
        }
        double det = Sw * Sss - Ss * Ss;
        if (Sw <= 0) break;
        if (det > 1e-9 * Sw * Sss) { m.A = (Sw * Ssv - Ss * Sv) / det; m.B = (Sv - m.A * Ss) / Sw; }
        else { m.A = 0; m.B = Sv / Sw; }
        if (m.A < 0) { m.A = 0; m.B = Sv / Sw; }
        m.B = std::max(m.B, 1.0);
        m.ok = true;
        for (size_t i = 0; i < bv.size(); i++) {
            double pred = m.A * bv[i].s + m.B, u = (bv[i].var - pred) / pred;  // relative excess
            w[i] = std::fabs(u) < 1 ? bv[i].n * (1 - u * u) * (1 - u * u) : 0;
        }
    }
    return m;
}

// Compact per-frame storage for median merging: 4 planes as uint16, value*2 + 4096
// (0.5 DN steps, -2048..30719 DN); 0 marks a sample the frame does not cover.
std::vector<uint16_t> encode(const Planes &P, const std::vector<uint8_t> *valid) {
    const size_t plane = size_t(P.w) * P.h;
    std::vector<uint16_t> s(4 * plane, 0);
    parallel_for(P.h, [&](int y0, int y1) {
        for (size_t o = size_t(y0) * P.w; o < size_t(y1) * P.w; o++) {
            if (valid && !(*valid)[o]) continue;
            for (int k = 0; k < 4; k++)
                s[k * plane + o] = uint16_t(std::clamp(std::lround(P.p[k].d[o] * 2.0f) + 4096, 1L, 65535L));
        }
    });
    return s;
}
inline float decode_sample(uint16_t u) { return (float(u) - 4096.0f) * 0.5f; }

double exposure_scale(const FrameInfo &ref, const FrameInfo &f) {
    auto ev = [](const FrameInfo &i) {
        double t = i.exposure > 0 ? i.exposure : 1, n = i.fnumber > 0 ? i.fnumber : 1, s = i.iso ? i.iso : 100;
        return t * s / (n * n);
    };
    double r = ev(ref) / ev(f);
    return std::isfinite(r) && r > 0.25 && r < 4 ? r : 1.0;
}

} // namespace

bool stack_frames(const File &f, const std::vector<size_t> &frames, size_t ref, const StackOptions &opt,
                  StackResult &out, std::string &err, const StackLog &log) {
    g_threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    auto decode = [&](size_t idx, Frame &fr, std::vector<uint16_t> &raw) {
        if (!f.read_frame(idx, fr, err)) return false;
        if (!nefcomp::he_decode(fr.codestream(), fr.cs_len, fr.width, fr.height, raw, err)) return false;
        return true;
    };
    std::vector<uint16_t> raw;
    Frame &rf = out.ref_frame;
    if (!decode(ref, rf, raw)) { err = "frame " + std::to_string(ref) + ": " + err; return false; }
    const unsigned W = rf.width, H = rf.height;
    if ((W | H) & 1) { err = "odd frame size"; return false; }
    const float black = float(black_level(rf));
    const unsigned clip_level = opt.white > 512 ? opt.white - 256 : opt.white;
    const FrameInfo ref_info = frame_info(rf);

    Planes R = split(raw, W, H, black, 1.0f, clip_level);
    Img rg = grey(R);
    std::vector<Img> rpyr = opt.align ? pyramid(rg) : std::vector<Img>{};
    Img rlp = box3(rg);

    // accumulators: start with the reference at weight 1 (clipped reference quads stay reference-only)
    Planes acc = R;
    Img wsum(R.w, R.h, 1.0f);
    const bool median = opt.merge == StackMerge::Median;
    std::vector<std::vector<uint16_t>> store;  // median: every aligned frame, reference first
    if (median) store.push_back(encode(R, nullptr));
    NoiseModel nm;
    size_t used = 1;
    Planes F, Wp;
    std::vector<uint8_t> valid;
    for (size_t idx : frames) {
        if (idx == ref) continue;
        Frame fr;
        if (!decode(idx, fr, raw)) { log("frame " + std::to_string(idx) + ": " + err + ", skipped"); continue; }
        if (fr.width != W || fr.height != H) { log("frame " + std::to_string(idx) + ": size differs, skipped"); continue; }
        const double sc = exposure_scale(ref_info, frame_info(fr));
        F = split(raw, W, H, float(black_level(fr)), float(sc), clip_level);
        Field fd;
        if (opt.align) fd = align(rpyr, pyramid(grey(F)), int(opt.tile));
        warp(F, opt.align ? &fd : nullptr, Wp, valid);
        double wtot = 0;
        if (median) {
            store.push_back(encode(Wp, &valid));
            for (uint8_t v : valid) wtot += v;
        } else {
        Img wlp = box3(grey(Wp));
        if (!nm.ok) {
            nm = fit_noise(rlp, wlp, valid);
            if (nm.ok) {
                char b[128];
                snprintf(b, sizeof b, "noise model: sigma(s) = sqrt(%.3f*s + %.1f) DN (low-passed grey difference)", nm.A, nm.B);
                log(b);
            }
        }
        // per-quad robust weight, then a 3x3 minimum so motion edges are rejected as a whole
        Img w(R.w, R.h, 0.0f);
        parallel_for(R.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < R.w; x++) {
                    size_t o = size_t(y) * R.w + x;
                    if (!valid[o] || R.clip[o]) continue;
                    double z = std::fabs(wlp.d[o] - rlp.d[o]) / nm.sigma(rlp.d[o]);
                    w.d[o] = float(std::clamp((opt.z_hi - z) / (opt.z_hi - opt.z_lo), 0.0, 1.0));
                }
        });
        Img wm(R.w, R.h);
        parallel_for(R.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < R.w; x++) {
                    float m = 1;
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++)
                            m = std::min(m, w.at(std::clamp(x + i, 0, R.w - 1), std::clamp(y + j, 0, R.h - 1)));
                    wm.at(x, y) = m;
                }
        });
        parallel_for(R.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < R.w; x++) {
                    size_t o = size_t(y) * R.w + x;
                    float ww = wm.d[o];
                    if (ww <= 0) continue;
                    for (int k = 0; k < 4; k++) acc.p[k].d[o] += ww * Wp.p[k].d[o];
                    wsum.d[o] += ww;
                }
        });
        for (float v : wm.d) wtot += v;
        }
        used++;
        // report the median displacement and mean weight
        char b[160];
        float mdx = 0, mdy = 0;
        if (opt.align) {
            std::vector<float> ax = fd.dx, ay = fd.dy;
            std::nth_element(ax.begin(), ax.begin() + long(ax.size() / 2), ax.end());
            std::nth_element(ay.begin(), ay.begin() + long(ay.size() / 2), ay.end());
            mdx = 2 * ax[ax.size() / 2]; mdy = 2 * ay[ay.size() / 2];  // sensor pixels
        }
        snprintf(b, sizeof b, "frame %4zu: shift %+6.1f %+6.1f px, exposure x%.3f, %s %5.1f %%", idx, mdx, mdy, sc,
                 median ? "usable" : "merged", 100.0 * wtot / double(valid.size()));
        log(b);
    }

    if (median) {
        // per-sample median over the frames that cover it (the reference always does)
        parallel_for(R.h, [&](int y0, int y1) {
            std::vector<float> v(store.size());
            const size_t plane = size_t(R.w) * R.h;
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < R.w; x++) {
                    size_t o = size_t(y) * R.w + x;
                    if (R.clip[o]) continue;  // clipped reference quad: keep the reference
                    for (int k = 0; k < 4; k++) {
                        size_t n = 0;
                        for (auto &s : store)
                            if (uint16_t u = s[k * plane + o]) v[n++] = decode_sample(u);
                        auto mid = v.begin() + long(n / 2);
                        std::nth_element(v.begin(), mid, v.begin() + long(n));
                        float m = *mid;
                        if (!(n & 1)) m = 0.5f * (m + *std::max_element(v.begin(), mid));
                        acc.p[k].d[o] = m;
                    }
                    wsum.d[o] = 1;
                }
        });
    }

    // output: scaled 16-bit mosaic
    const float S = float(opt.out_scale);
    out.width = W; out.height = H;
    out.black = unsigned(std::lround(black * S));
    out.white = unsigned(std::min(65535.0f, std::round((float(opt.white) - black) * S + black * S)));
    out.px.assign(size_t(W) * H, 0);
    parallel_for(R.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < R.w; x++) {
                size_t o = size_t(y) * R.w + x;
                for (int k = 0; k < 4; k++) {
                    float v = acc.p[k].d[o] / wsum.d[o];
                    float q = std::round(v * S + black * S);
                    out.px[size_t(2 * y + (k >> 1)) * W + 2 * x + (k & 1)] = uint16_t(std::clamp(q, 0.0f, 65535.0f));
                }
            }
    });
    out.frames_used = used;
    return true;
}

} // namespace nev
