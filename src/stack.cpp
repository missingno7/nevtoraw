// SPDX-License-Identifier: GPL-2.0-or-later
#include "stack.h"
#include "sr.h"

#include "he_decode.h"
#include "meta.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
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
        std::fill(fd.dx.begin(), fd.dx.end(), float(gx * (1 << L)));
        std::fill(fd.dy.begin(), fd.dy.end(), float(gy * (1 << L)));
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

struct ControlImage {
    Img fine, coarse;
};

ControlImage control_image(Img grey) {
    // Compress bright features and suppress noise for matching only.
    for (float &v : grey.d) v = std::sqrt(std::max(v + 20.0f, 1.0f));
    Img fine = box3(grey);
    Img coarse = down2(fine);
    return {std::move(fine), std::move(coarse)};
}

struct ControlPoint { int x, y; };

std::vector<ControlPoint> control_points(const Img &image) {
    const int cell = 88, margin = 24;
    std::vector<ControlPoint> points;
    for (int top = margin; top < image.h - margin; top += cell)
        for (int left = margin; left < image.w - margin; left += cell) {
            ControlPoint best{};
            double strength = 0.24;
            for (int y = top; y < std::min(top + cell, image.h - margin); y += 7)
                for (int x = left; x < std::min(left + cell, image.w - margin); x += 7) {
                    double xx = 0, xy = 0, yy = 0;
                    for (int j = -8; j <= 8; j += 2)
                        for (int i = -8; i <= 8; i += 2) {
                            double gx = 0.5 * (image.at(x + i + 1, y + j) - image.at(x + i - 1, y + j));
                            double gy = 0.5 * (image.at(x + i, y + j + 1) - image.at(x + i, y + j - 1));
                            xx += gx * gx; xy += gx * gy; yy += gy * gy;
                        }
                    // The smaller gradient eigenvalue selects features that constrain both axes.
                    const double score = (xx + yy - std::hypot(xx - yy, 2 * xy)) / (2 * 81);
                    if (score > strength) { strength = score; best = {x, y}; }
                }
            if (best.x) points.push_back(best);
        }
    return points;
}

struct Patch {
    const Img &image;
    int x, y, radius;
    double sum = 0, variance = 0;

    Patch(const Img &image, int x, int y, int radius) : image(image), x(x), y(y), radius(radius) {
        double square_sum = 0;
        for (int j = -radius; j <= radius; ++j)
            for (int i = -radius; i <= radius; ++i) {
                double v = image.at(x + i, y + j);
                sum += v; square_sum += v * v;
            }
        variance = square_sum - sum * sum / count();
    }

    int count() const { return (2 * radius + 1) * (2 * radius + 1); }

    double correlation(const Img &other, int ox, int oy) const {
        if (ox - radius < 0 || oy - radius < 0 || ox + radius >= other.w || oy + radius >= other.h)
            return -1;
        double other_sum = 0, other_square_sum = 0, product_sum = 0;
        for (int j = -radius; j <= radius; ++j) {
            const float *a = &image.d[size_t(y + j) * image.w + x];
            const float *b = &other.d[size_t(oy + j) * other.w + ox];
            for (int i = -radius; i <= radius; ++i) {
                double v = b[i];
                other_sum += v; other_square_sum += v * v; product_sum += a[i] * v;
            }
        }
        double other_variance = other_square_sum - other_sum * other_sum / count();
        if (variance < 1e-8 || other_variance < 1e-8) return -1;
        return (product_sum - sum * other_sum / count()) / std::sqrt(variance * other_variance);
    }
};

struct Peak { int x, y; double score, sub_x = 0, sub_y = 0; };

Peak match_patch(const Patch &patch, const Img &image, int cx, int cy, int radius, bool subpixel) {
    Peak best{cx, cy, -1};
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            double score = patch.correlation(image, x, y);
            if (score > best.score) best = {x, y, score};
        }
    if (subpixel && best.score > 0) {
        auto offset = [&](double left, double right) {
            double curvature = left + right - 2 * best.score;
            return left > -1 && right > -1 && curvature < 0
                       ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
        };
        auto score = [&](int dx, int dy) { return patch.correlation(image, best.x + dx, best.y + dy); };
        best.sub_x = offset(score(-1, 0), score(1, 0));
        best.sub_y = offset(score(0, -1), score(0, 1));
    }
    return best;
}

struct Match { double x = 0, y = 0, dx = 0, dy = 0; bool valid = false; };

std::vector<Match> match_controls(const ControlImage &ref, const ControlImage &frame, const Field &prior,
                                  const std::vector<ControlPoint> &points) {
    std::vector<Match> matches(points.size());
    parallel_for(int(points.size()), [&](int start, int end) {
        for (int i = start; i < end; ++i) {
            auto [x, y] = points[size_t(i)];
            float dx, dy;
            prior.bilinear(float(x), float(y), dx, dy);
            auto coarse = match_patch(Patch(ref.coarse, x / 2, y / 2, 10), frame.coarse,
                                      x / 2 + int(std::lround(dx / 2)), y / 2 + int(std::lround(dy / 2)), 14, false);
            if (coarse.score < 0.72) continue;
            auto fine = match_patch(Patch(ref.fine, x, y, 16), frame.fine,
                                    2 * coarse.x + (x & 1), 2 * coarse.y + (y & 1), 3, true);
            if (fine.score < 0.86) continue;
            // Require forward/backward agreement to reject ambiguous and occluded matches.
            auto back = match_patch(Patch(frame.fine, fine.x, fine.y, 16), ref.fine, x, y, 4, true);
            if (back.score < 0.85 || std::hypot(back.x - x + back.sub_x + fine.sub_x,
                                              back.y - y + back.sub_y + fine.sub_y) > 0.55) continue;
            matches[size_t(i)] = {double(x), double(y), fine.x - x + fine.sub_x, fine.y - y + fine.sub_y, true};
        }
    });
    matches.erase(std::remove_if(matches.begin(), matches.end(), [](const Match &m) { return !m.valid; }), matches.end());
    return matches;
}

// Inverse warp (reference to source) in CFA-plane coordinates. The fit uses centred,
// scaled coordinates and models displacement so the solution is well conditioned.
struct Homography {
    double cx = 0, cy = 0, scale = 1;
    std::array<double, 8> a{};

    double denominator(double x, double y) const {
        return 1 + (a[6] * (x - cx) + a[7] * (y - cy)) / (scale * scale);
    }

    void displacement(double x, double y, double &dx, double &dy) const {
        const double u = (x - cx) / scale, v = (y - cy) / scale;
        const double t = a[6] * u + a[7] * v, w = 1 + t / scale;
        dx = (a[0] + a[2] * u + a[3] * v - t * u) / w;
        dy = (a[1] + a[4] * u + a[5] * v - t * v) / w;
    }

    double error_squared(const Match &m) const {
        double dx, dy;
        displacement(m.x, m.y, dx, dy);
        dx -= m.dx; dy -= m.dy;
        return dx * dx + dy * dy;
    }
};

bool solve_homography(Homography &h, const std::vector<Match> &matches, const std::vector<double> &weights) {
    double matrix[8][9] = {};
    for (size_t i = 0; i < matches.size(); ++i) {
        if (weights[i] == 0) continue;
        const auto &m = matches[i];
        const double x = (m.x - h.cx) / h.scale, y = (m.y - h.cy) / h.scale;
        const double u = x + m.dx / h.scale, v = y + m.dy / h.scale;
        const double rows[2][9] = {
            {1, 0, x, y, 0, 0, -u * x, -u * y, m.dx},
            {0, 1, 0, 0, x, y, -v * x, -v * y, m.dy},
        };
        for (const auto &row : rows)
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 9; ++c)
                    matrix[r][c] += weights[i] * row[r] * row[c];
    }
    for (int c = 0; c < 8; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 8; ++r)
            if (std::abs(matrix[r][c]) > std::abs(matrix[pivot][c])) pivot = r;
        if (std::abs(matrix[pivot][c]) < 1e-9) return false;
        std::swap(matrix[c], matrix[pivot]);
        for (int r = 0; r < 8; ++r) {
            if (r == c) continue;
            double factor = matrix[r][c] / matrix[c][c];
            for (int j = c; j < 9; ++j) matrix[r][j] -= factor * matrix[c][j];
        }
    }
    for (int i = 0; i < 8; ++i) h.a[i] = matrix[i][8] / matrix[i][i];
    return true;
}

bool fit_homography(const std::vector<Match> &matches, int width, int height, Homography &h, size_t &inliers) {
    constexpr size_t min_inliers = 12;
    constexpr double cutoff = 1.75 * 1.75;  // squared plane pixels
    if (matches.size() < min_inliers) return false;
    h = {(width - 1) * 0.5, (height - 1) * 0.5, std::max(width, height) * 0.5};
    std::vector<double> weights(matches.size());
    std::mt19937 rng(151);
    double best_loss = INFINITY;
    for (int trial = 0; trial < 300; ++trial) {
        Homography candidate = h;
        std::fill(weights.begin(), weights.end(), 0);
        for (int n = 0; n < 6; ++n) weights[rng() % matches.size()] = 1;
        if (!solve_homography(candidate, matches, weights)) continue;
        double loss = 0;
        for (const auto &m : matches) loss += std::min(cutoff, candidate.error_squared(m));
        if (loss < best_loss) { h = candidate; best_loss = loss; }
    }
    if (!std::isfinite(best_loss)) return false;
    for (double radius : {4.0, 3.0, 2.5, 2.0, 2.0, 2.0}) {
        for (size_t i = 0; i < matches.size(); ++i) {
            double e = h.error_squared(matches[i]) / (radius * radius);
            weights[i] = e < 1 ? (1 - e) * (1 - e) : 0;
        }
        if (!solve_homography(h, matches, weights)) break;
    }
    int left = width, right = 0, top = height, bottom = 0;
    for (const auto &m : matches) {
        if (!(h.error_squared(m) <= cutoff)) continue;
        ++inliers;
        left = std::min(left, int(m.x)); right = std::max(right, int(m.x));
        top = std::min(top, int(m.y)); bottom = std::max(bottom, int(m.y));
    }
    if (inliers < min_inliers || inliers * 2 < matches.size() ||
        right - left < width / 4 || bottom - top < height / 5) return false;
    // The denominator is linear over the image. Require it to stay clear of zero
    // throughout the footprint so the warp has no pole inside the frame.
    for (int y : {0, height - 1})
        for (int x : {0, width - 1})
            if (!(h.denominator(x, y) > 0.1)) return false;
    return true;
}

inline void catmull(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2 * t2 - t);
    w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

struct Footprint {
    int x, y;
    float wx[4], wy[4];
};

bool make_footprint(const Planes &src, float x, float y, Footprint &f) {
    f.x = int(std::floor(x)); f.y = int(std::floor(y));
    if (f.x < 1 || f.y < 1 || f.x + 2 >= src.w || f.y + 2 >= src.h) return false;
    catmull(x - f.x, f.wx); catmull(y - f.y, f.wy);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            if (f.wx[i] != 0 && f.wy[j] != 0 && src.clip[size_t(f.y - 1 + j) * src.w + f.x - 1 + i])
                return false;
    return true;
}

float interpolate(const Img &img, const Footprint &f) {
    float acc = 0;
    for (int j = 0; j < 4; ++j) {
        const float *r = &img.d[size_t(f.y - 1 + j) * img.w + f.x - 1];
        acc += f.wy[j] * (f.wx[0] * r[0] + f.wx[1] * r[1] + f.wx[2] * r[2] + f.wx[3] * r[3]);
    }
    return acc;
}

// Resample the four planes separately; valid marks quads with an unclipped footprint.
void warp(const Planes &src, const Field *fd, const Homography *h, Planes &dst, std::vector<uint8_t> &valid) {
    dst.w = src.w; dst.h = src.h;
    for (auto &i : dst.p) if (i.w != src.w || i.h != src.h) i = Img(src.w, src.h);
    valid.assign(size_t(src.w) * src.h, 0);
    for (auto &i : dst.p) std::fill(i.d.begin(), i.d.end(), 0.0f);
    parallel_for(src.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < src.w; x++) {
                size_t o = size_t(y) * src.w + x;
                if (h) {
                    Footprint sites[4];
                    bool inside = true;
                    for (int k = 0; k < 4 && inside; ++k) {
                        // Matching coordinates refer to the centre of the Bayer quad.
                        // Evaluate the transform at each site's own position in that quad.
                        double dx, dy;
                        h->displacement(x + ((k & 1) ? 0.25 : -0.25),
                                        y + ((k & 2) ? 0.25 : -0.25), dx, dy);
                        inside = make_footprint(src, float(x + dx), float(y + dy), sites[k]);
                    }
                    if (!inside) continue;
                    for (int k = 0; k < 4; ++k) dst.p[k].d[o] = interpolate(src.p[k], sites[k]);
                } else if (fd) {
                    float dx, dy;
                    fd->bilinear(float(x), float(y), dx, dy);
                    Footprint footprint;
                    if (!make_footprint(src, x + dx, y + dy, footprint)) continue;
                    for (int k = 0; k < 4; ++k) dst.p[k].d[o] = interpolate(src.p[k], footprint);
                } else {
                    for (int k = 0; k < 4; k++) dst.p[k].d[o] = src.p[k].d[o];
                    valid[o] = !src.clip[o];
                    continue;
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

// Compact per-frame storage for per-sample merging: 4 planes as uint16, value*2 + 4096
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

float trimmed_mean(uint16_t *begin, uint16_t *end, unsigned percent) {
    const auto trim = (end - begin) * percent / 100;
    auto *low = begin + trim, *high = end - trim;
    if (trim) {
        std::nth_element(begin, low, end);
        std::nth_element(low, high, end);
    }
    const auto sum = std::accumulate(low, high, uint64_t{0});
    return float((double(sum) / (high - low) - 4096.0) * 0.5);
}

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
    const bool trimmed = opt.merge == StackMerge::TrimmedMean;
    if (trimmed && opt.trim_percent > 49) { err = "trim percentage must be 0..49"; return false; }
    const bool per_sample = opt.merge != StackMerge::Mean;
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
    const bool aligned = opt.alignment != StackAlignment::None;
    const bool projective = opt.alignment == StackAlignment::Homography;

    Planes R = split(raw, W, H, black, 1.0f, clip_level);
    Img rg = grey(R);
    auto rpyr = aligned ? pyramid(rg) : std::vector<Img>{};
    ControlImage ref_control = projective ? control_image(rg) : ControlImage{};
    auto points = projective ? control_points(ref_control.fine) : std::vector<ControlPoint>{};
    Img rlp = per_sample ? Img{} : box3(rg);

    // accumulators: start with the reference at weight 1 (clipped reference quads stay reference-only)
    Planes acc = R;
    Img wsum(R.w, R.h, 1.0f);
    std::vector<std::vector<uint16_t>> store; // every aligned frame, reference first
    if (per_sample) store.push_back(encode(R, nullptr));
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
        Homography h;
        size_t inliers = 0, matched = 0;
        if (aligned) {
            Img g = grey(F);
            fd = align(rpyr, pyramid(g), int(opt.tile));
            if (projective) {
                auto matches = match_controls(ref_control, control_image(std::move(g)), fd, points);
                matched = matches.size();
                if (!fit_homography(matches, R.w, R.h, h, inliers)) {
                    log("frame " + std::to_string(idx) + ": insufficient homography support (" +
                        std::to_string(inliers) + "/" + std::to_string(matched) + " matches), skipped");
                    continue;
                }
            }
        }
        warp(F, aligned && !projective ? &fd : nullptr, projective ? &h : nullptr, Wp, valid);
        double wtot = 0;
        if (per_sample) {
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
        // Report the centre displacement for a homography, the median displacement for tiles.
        char b[192];
        float mdx = 0, mdy = 0;
        if (projective) {
            double dx, dy;
            h.displacement(h.cx, h.cy, dx, dy);
            mdx = float(2 * dx); mdy = float(2 * dy);
        } else if (aligned) {
            std::vector<float> ax = fd.dx, ay = fd.dy;
            std::nth_element(ax.begin(), ax.begin() + long(ax.size() / 2), ax.end());
            std::nth_element(ay.begin(), ay.begin() + long(ay.size() / 2), ay.end());
            mdx = 2 * ax[ax.size() / 2]; mdy = 2 * ay[ay.size() / 2];  // sensor pixels
        }
        snprintf(b, sizeof b, "frame %4zu: shift %+6.1f %+6.1f px, exposure x%.3f, %s %5.1f %%", idx, mdx, mdy, sc,
                 per_sample ? "usable" : "merged", 100.0 * wtot / double(valid.size()));
        std::string message = b;
        if (projective) message += ", control points " + std::to_string(inliers) + "/" + std::to_string(matched);
        log(message);
    }

    if (projective && used == 1 && std::any_of(frames.begin(), frames.end(), [ref](size_t idx) { return idx != ref; })) {
        err = "no other frame could be aligned to reference " + std::to_string(ref) + " with a homography";
        return false;
    }

    if (per_sample) {
        // Merge the frames that cover each sample (the reference always does).
        parallel_for(R.h, [&](int y0, int y1) {
            std::vector<uint16_t> values(store.size());
            const size_t plane = size_t(R.w) * R.h;
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < R.w; x++) {
                    size_t o = size_t(y) * R.w + x;
                    if (R.clip[o]) continue;  // clipped reference quad: keep the reference
                    for (int k = 0; k < 4; k++) {
                        size_t n = 0;
                        for (auto &s : store)
                            if (uint16_t u = s[k * plane + o]) values[n++] = u;
                        if (trimmed) {
                            acc.p[k].d[o] = trimmed_mean(values.data(), values.data() + n, opt.trim_percent);
                            continue;
                        }
                        auto mid = values.begin() + long(n / 2);
                        std::nth_element(values.begin(), mid, values.begin() + long(n));
                        float m = decode_sample(*mid);
                        if (!(n & 1)) m = 0.5f * (m + decode_sample(*std::max_element(values.begin(), mid)));
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

namespace {

struct Position { double x = 0, y = 0; };

struct ProjectiveMap {
    double cx = 0, cy = 0, scale = 1;
    std::array<double, 9> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};

    Position at(double x, double y) const {
        double u = (x - cx) / scale, v = (y - cy) / scale;
        const auto &m = matrix;
        double gain = scale / (m[6] * u + m[7] * v + m[8]);
        return {cx + gain * (m[0] * u + m[1] * v + m[2]),
                cy + gain * (m[3] * u + m[4] * v + m[5])};
    }

    ProjectiveMap inverse() const {
        const auto &m = matrix;
        return {cx, cy, scale, {
             m[4]*m[8]-m[5]*m[7], m[2]*m[7]-m[1]*m[8], m[1]*m[5]-m[2]*m[4],
             m[5]*m[6]-m[3]*m[8], m[0]*m[8]-m[2]*m[6], m[2]*m[3]-m[0]*m[5],
             m[3]*m[7]-m[4]*m[6], m[1]*m[6]-m[0]*m[7], m[0]*m[4]-m[1]*m[3],
        }};
    }
};

ProjectiveMap sensor_map(const Homography &h, Position residual) {
    const auto &a = h.a;
    const double s = h.scale, u = residual.x / s, v = residual.y / s;
    const double g = a[6] / s, k = a[7] / s;
    // The alignment proxy treats the midpoint of each four-site quad as its pixel centre.
    return {2 * h.cx + .5, 2 * h.cy + .5, 2 * s, {
        1 + a[2]/s + u*g, a[3]/s + u*k, a[0]/s + u,
        a[4]/s + v*g, 1 + a[5]/s + v*k, a[1]/s + v,
        g, k, 1,
    }};
}

Position refine_crop(const Img &reference, const Img &frame, const Homography &model, const SrCrop &crop) {
    Position shift;
    const int left = int(crop.x / 2), top = int(crop.y / 2);
    const int right = int((crop.x + crop.width) / 2), bottom = int((crop.y + crop.height) / 2);
    const int step = std::max(2, std::min(right - left, bottom - top) / 100);
    for (int iteration = 0; iteration < 8; ++iteration) {
        double xx = 0, xy = 0, yy = 0, rx = 0, ry = 0;
        for (int y = top; y < bottom; y += step)
            for (int x = left; x < right; x += step) {
                double dx, dy;
                model.displacement(x, y, dx, dy);
                double u = x + dx + shift.x, v = y + dy + shift.y;
                int ix = int(std::floor(u)), iy = int(std::floor(v));
                if (ix < 0 || iy < 0 || ix + 1 >= frame.w || iy + 1 >= frame.h) continue;
                double fx = u - ix, fy = v - iy;
                double a = frame.at(ix, iy), b = frame.at(ix + 1, iy);
                double c = frame.at(ix, iy + 1), d = frame.at(ix + 1, iy + 1);
                double upper = a + fx * (b - a), lower = c + fx * (d - c);
                double gx = (1 - fy) * (b - a) + fy * (d - c), gy = lower - upper;
                double e = reference.at(x, y) - (upper + fy * gy);
                double w = 1 / std::max(1.0, std::abs(e) / 2);
                xx += w * gx * gx; xy += w * gx * gy; yy += w * gy * gy;
                rx += w * gx * e; ry += w * gy * e;
            }
        double det = xx * yy - xy * xy;
        if (det < 1e-9) break;
        double dx = (yy * rx - xy * ry) / det, dy = (xx * ry - xy * rx) / det;
        shift.x += std::clamp(dx, -.75, .75);
        shift.y += std::clamp(dy, -.75, .75);
        if (std::hypot(dx, dy) < .005) break;
    }
    return shift;
}

struct RegisteredPhoto {
    size_t index;
    RawPhoto photo;
    ProjectiveMap forward, backward;
    float exposure = 1;
};

// Pick different sampling phases first so smaller subsets cover the Bayer period.
void order_by_phase(std::vector<RegisteredPhoto> &frames, const SrCrop &crop) {
    const double x = crop.x + .5 * crop.width, y = crop.y + .5 * crop.height;
    auto distance = [](Position a, Position b) {
        const double dx = std::remainder(a.x - b.x, 2), dy = std::remainder(a.y - b.y, 2);
        return dx * dx + dy * dy;
    };
    std::vector<Position> phases;
    for (const auto &f : frames) phases.push_back(f.forward.at(x, y));
    std::vector<double> nearest(frames.size(), 8);
    for (size_t i = 1; i < frames.size(); ++i) {
        for (size_t j = i; j < frames.size(); ++j)
            nearest[j] = std::min(nearest[j], distance(phases[i - 1], phases[j]));
        size_t best = size_t(std::max_element(nearest.begin() + i, nearest.end()) - nearest.begin());
        std::swap(frames[i], frames[best]);
        std::swap(phases[i], phases[best]);
        std::swap(nearest[i], nearest[best]);
    }
}

struct Moments {
    float sum = 0, x = 0, y = 0, xx = 0, xy = 0, yy = 0;
    float value = 0, xv = 0, yv = 0;

    void add(float w, float dx, float dy, float v) {
        float wx = w * dx, wy = w * dy, wv = w * v;
        sum += w; x += wx; y += wy;
        xx += wx * dx; xy += wx * dy; yy += wy * dy;
        value += wv; xv += wv * dx; yv += wv * dy;
    }

    float estimate() const {
        const float dx = x / sum, dy = y / sum, mean = value / sum;
        const float a = xx / sum - dx * dx + .015f;
        const float b = xy / sum - dx * dy;
        const float c = yy / sum - dy * dy + .015f;
        const float u = xv / sum - dx * mean, v = yv / sum - dy * mean;
        return mean - ((c * u - b * v) * dx + (a * v - b * u) * dy) / (a * c - b * b);
    }
};

float reference_color(const RawPhoto &p, int color, double x, double y) {
    // Bilinear interpolation on the reference's same-color sites for sparse
    // coverage, such as the outer margin or very short bursts.
    double result = 0;
    int count = 0;
    for (int site = 0; site < 4; ++site) {
        if (p.info.cfa[site] != unsigned(color)) continue;
        const int ox = site & 1, oy = site >> 1;
        double px = std::clamp((x - ox) * .5, 0.0, double(p.width / 2 - 1));
        double py = std::clamp((y - oy) * .5, 0.0, double(p.height / 2 - 1));
        int ax = int(px), ay = int(py);
        int bx = std::min(ax + 1, int(p.width / 2) - 1), by = std::min(ay + 1, int(p.height / 2) - 1);
        const double fx = px - ax, fy = py - ay;
        auto at = [&](int u, int v) { return p.pixels[size_t(2*v + oy) * p.width + 2*u + ox]; };
        result += (1-fy) * ((1-fx) * at(ax, ay) + fx * at(bx, ay)) +
                  fy * ((1-fx) * at(ax, by) + fx * at(bx, by));
        ++count;
    }
    return float(result / count - p.black);
}

constexpr float sr_radius = 1.20f;
constexpr float sr_radius_squared = sr_radius * sr_radius;
using Kernel = std::array<float, 1025>;

void accumulate_tile(const RegisteredPhoto &frame, int left, int top, int width, int height,
                     const Kernel &kernel, std::vector<Moments> &moment) {
    const auto &p = frame.photo;
    Position low{double(p.width), double(p.height)}, high{-1, -1};
    for (int x : {left - 2, left + width / 2 + 2})
        for (int y : {top - 2, top + height / 2 + 2}) {
            Position s = frame.forward.at(x, y);
            low.x = std::min(low.x, s.x); low.y = std::min(low.y, s.y);
            high.x = std::max(high.x, s.x); high.y = std::max(high.y, s.y);
        }
    const int first_x = std::max(0, int(std::floor(low.x))), first_y = std::max(0, int(std::floor(low.y)));
    const int last_x = std::min(int(p.width) - 1, int(std::ceil(high.x)));
    const int last_y = std::min(int(p.height) - 1, int(std::ceil(high.y)));
    constexpr float radius = 2 * sr_radius;
    for (int y = first_y; y <= last_y; ++y)
        for (int x = first_x; x <= last_x; ++x) {
            Position pos = frame.backward.at(x, y);
            float cx = float(2 * (pos.x - left) + .5), cy = float(2 * (pos.y - top) + .5);
            const unsigned color = p.info.cfa[(y & 1) * 2 + (x & 1)];
            const float value = (float(p.pixels[size_t(y) * p.width + x]) - p.black) * frame.exposure;
            const int y0 = std::max(0, int(std::ceil(cy - radius))), y1 = std::min(height - 1, int(std::floor(cy + radius)));
            const int x0 = std::max(0, int(std::ceil(cx - radius))), x1 = std::min(width - 1, int(std::floor(cx + radius)));
            for (int oy = y0; oy <= y1; ++oy)
                for (int ox = x0; ox <= x1; ++ox) {
                    float dx = .5f * (cx - ox), dy = .5f * (cy - oy);
                    float d2 = dx * dx + dy * dy;
                    if (d2 > sr_radius_squared) continue;
                    auto weight = kernel[size_t(d2 * ((kernel.size() - 1) / sr_radius_squared))];
                    moment[3 * (size_t(oy) * width + ox) + color].add(weight, dx, dy, value);
                }
        }
}

void reconstruct_sr(const std::vector<RegisteredPhoto> &frames, const SrCrop &crop, const FrameInfo &info,
                    const std::vector<size_t> &counts, std::vector<SrImage> &results) {
    constexpr int tile = 128;
    const int width = int(2 * crop.width), height = int(2 * crop.height);
    const int columns = (int(crop.width) + tile - 1) / tile, rows = (int(crop.height) + tile - 1) / tile;
    const auto &ref = frames.front().photo;
    const float scale = float(65535 / ref.white);
    Kernel kernel;
    for (size_t i = 0; i < kernel.size(); ++i) {
        constexpr float sigma = .42f;
        float d2 = float(i) * sr_radius_squared / float(kernel.size() - 1);
        kernel[i] = std::exp(-d2 / (2 * sigma * sigma));
    }
    parallel_for(columns * rows, [&](int first, int last) {
        for (int index = first; index < last; ++index) {
            const int x0 = 2 * tile * (index % columns), y0 = 2 * tile * (index / columns);
            const int w = std::min(width - x0, 2 * tile), h = std::min(height - y0, 2 * tile);
            const int left = int(crop.x) + x0 / 2, top = int(crop.y) + y0 / 2;
            std::vector<Moments> moment(size_t(w) * h * 3);
            size_t next = 0;
            for (size_t n = 0; n < frames.size(); ++n) {
                accumulate_tile(frames[n], left, top, w, h, kernel, moment);
                if (n + 1 != counts[next]) continue;
                SrImage &out = results[next];
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = size_t(y) * w + x, o = size_t(y + y0) * out.width + x + x0;
                        const unsigned site = info.cfa[(y & 1) * 2 + (x & 1)];
                        for (unsigned c = 0; c < 3; ++c) {
                            if (!out.linear && c != site) continue;
                            const auto &m = moment[3 * i + c];
                            float v = m.sum > 1e-6f ? m.estimate() : 0;
                            if (m.sum < .25f) {
                                float base = reference_color(ref, int(c), left + .5 * x - .25, top + .5 * y - .25);
                                float w = 4 * m.sum;
                                v = w * v + (1 - w) * base;
                            }
                            uint16_t packed = uint16_t(std::lround(std::clamp((v + ref.black) * scale, 0.0f, float(out.white))));
                            out.pixels[out.linear ? 3 * o + c : o] = packed;
                        }
                    }
                if (++next == counts.size()) break;
            }
        }
    });
}

} // namespace

bool superresolve(const ReadPhoto &read, const std::vector<size_t> &candidates, size_t reference,
                  const SrOptions &opt, const SaveSr &save, std::string &err, const StackLog &log) {
    RawPhoto ref;
    if (!read(reference, ref, err)) return false;
    std::array<unsigned, 4> colors;
    std::copy_n(ref.info.cfa, colors.size(), colors.begin());
    std::sort(colors.begin(), colors.end());
    if ((ref.width | ref.height) & 1 || colors != std::array<unsigned, 4>{0, 1, 1, 2} ||
        ref.pixels.size() != size_t(ref.width) * ref.height || ref.black >= ref.white || ref.white > 65535) {
        err = "invalid input geometry, Bayer pattern or raw levels"; return false;
    }
    const bool cropped = opt.crop.width != 0;
    SrCrop crop = cropped ? opt.crop : SrCrop{0, 0, ref.width, ref.height};
    if (crop.width < 2 || crop.height < 2 || crop.x > ref.width || crop.width > ref.width - crop.x ||
        crop.y > ref.height || crop.height > ref.height - crop.y) {
        err = "crop is outside the reference or smaller than 2 pixels"; return false;
    }
    if (size_t(crop.width) * crop.height * (opt.linear ? 24 : 8) > UINT32_MAX - (1u << 20)) {
        err = "output exceeds the DNG writer's 32-bit TIFF limit"; return false;
    }
    if (opt.counts.empty() || opt.counts.front() == 0 || opt.counts.back() > candidates.size() ||
        std::adjacent_find(opt.counts.begin(), opt.counts.end(), std::greater_equal<size_t>{}) != opt.counts.end()) {
        err = "requested frame count exceeds the selection"; return false;
    }
    g_threads = std::max(1u, std::thread::hardware_concurrency());
    const auto movie = ref.movie;
    FrameInfo info = ref.info;
    if (cropped)
        for (unsigned k = 0; k < 4; ++k)
            info.cfa[k] = ref.info.cfa[((k >> 1) ^ (crop.y & 1)) * 2 + ((k & 1) ^ (crop.x & 1))];
    auto crop_movie = [&](unsigned scale) {
        auto m = movie;
        m.crop_w = scale * (cropped ? crop.width : movie.crop_w);
        m.crop_h = scale * (cropped ? crop.height : movie.crop_h);
        return m;
    };
    std::vector<size_t> counts = opt.counts;
    if (counts.front() == 1) {
        SrImage single{crop.width, crop.height, ref.black, ref.white, false, {}};
        single.pixels.resize(size_t(crop.width) * crop.height);
        for (unsigned y = 0; y < crop.height; ++y)
            std::copy_n(ref.pixels.data() + size_t(crop.y + y) * ref.width + crop.x, crop.width,
                        single.pixels.data() + size_t(y) * crop.width);
        if (!save(1, single, crop_movie(1), info)) return false;
        counts.erase(counts.begin());
        if (counts.empty()) return true;
    }

    auto guide = grey(split(ref.pixels, ref.width, ref.height, float(ref.black), 1, ref.white));
    auto reference_pyramid = pyramid(guide);
    auto reference_controls = control_image(std::move(guide));
    auto points = control_points(reference_controls.fine);
    std::vector<RegisteredPhoto> frames;
    frames.reserve(counts.back());
    frames.push_back({reference, std::move(ref), {}, {}, 1});

    // Start with frames spread over the entire selected interval, then try the
    // remaining candidates if weaker frames could not be aligned.
    std::vector<size_t> order;
    const size_t n = counts.back();
    for (size_t i = 0; i < n; ++i) order.push_back(candidates[i * (candidates.size() - 1) / (n - 1)]);
    for (size_t i : candidates)
        if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
    for (size_t index : order) {
        if (frames.size() == n) break;
        if (index == reference) continue;
        RawPhoto raw;
        if (!read(index, raw, err)) { log(err + ", skipped"); continue; }
        const auto &base = frames.front().photo;
        if (raw.width != base.width || raw.height != base.height || raw.movie.model != base.movie.model ||
            !std::equal(std::begin(raw.info.cfa), std::end(raw.info.cfa), std::begin(base.info.cfa))) {
            log("frame " + std::to_string(index) + ": different raw geometry or CFA, skipped");
            continue;
        }
        const float exposure = float(exposure_scale(base.info, raw.info));
        Img g = grey(split(raw.pixels, raw.width, raw.height, float(raw.black), exposure, raw.white));
        auto prior = align(reference_pyramid, pyramid(g), 32);
        auto control = control_image(std::move(g));
        auto matches = match_controls(reference_controls, control, prior, points);
        Homography h;
        size_t inliers = 0;
        if (!fit_homography(matches, control.fine.w, control.fine.h, h, inliers)) {
            log("frame " + std::to_string(index) + ": insufficient homography support, skipped");
            continue;
        }
        auto residual = cropped ? refine_crop(reference_controls.fine, control.fine, h, crop) : Position{};
        auto warp = sensor_map(h, residual);
        frames.push_back({index, std::move(raw), warp, warp.inverse(), exposure});
        log("frame " + std::to_string(index) + ": " + std::to_string(inliers) + "/" +
            std::to_string(matches.size()) + " control points");
    }
    if (frames.size() != counts.back()) {
        err = "aligned " + std::to_string(frames.size()) + " of the requested " + std::to_string(counts.back()) + " frames";
        return false;
    }
    order_by_phase(frames, crop);
    std::string frame_order = "nested frame order:";
    for (const auto &frame : frames) frame_order += " " + std::to_string(frame.index);
    log(frame_order);

    const auto &base = frames.front().photo;
    const unsigned scale = 65535 / base.white;
    std::vector<SrImage> results;
    for (size_t i = 0; i < counts.size(); ++i) {
        SrImage out{2 * crop.width, 2 * crop.height, scale * base.black, scale * base.white, opt.linear, {}};
        out.pixels.resize(size_t(out.width) * out.height * (opt.linear ? 3 : 1));
        results.push_back(std::move(out));
    }
    reconstruct_sr(frames, crop, info, counts, results);
    frames.clear();
    for (size_t i = 0; i < results.size(); ++i)
        if (!save(counts[i], results[i], crop_movie(2), info)) return false;
    return true;
}

} // namespace nev
