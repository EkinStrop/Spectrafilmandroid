/*
 * Spektrafilm for Android — native engine: output gamut compression.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports utils/gamut_compression.py::reinhard_knee,
 * compress_rgb_aces_rgc, and compress_rgb_oklch_chroma (the OkLch output-gamut
 * chroma reduction). All math is in double precision to match the oracle
 * (NumPy float64); the knee uses std::pow, the same transcendental NumPy calls.
 */
#include "model/gamut_compression.h"

#include <cmath>
#include <limits>
#include <vector>

namespace spk {

double reinhard_knee(double d, double threshold, double limit, double power) {
    // gamut_compression.py::reinhard_knee: identity at/below threshold (the oracle's
    // `mask = d > threshold` is strict, so d == threshold returns d unchanged), and a
    // smooth Reinhard roll-off above it that asymptotes to `limit`.
    if (!(d > threshold)) return d;  // (!(>) also leaves NaN untouched, as np.where would)
    const double scale = limit - threshold;
    const double x = (d - threshold) / scale;
    const double y = x / std::pow(1.0 + std::pow(x, power), 1.0 / power);
    return threshold + scale * y;
}

void compress_pixel_aces_rgc(const double rgb[3], double threshold, double limit,
                             double power, double out[3]) {
    // gamut_compression.py::compress_rgb_aces_rgc, per pixel.
    //   ach = max(R,G,B)
    //   safe_ach = ach if ach > 1e-12 else 1.0
    //   d = (ach - c) / safe_ach   (per channel; d >= 0, and d > 1 iff c < 0)
    //   c' = ach * (1 - reinhard_knee(d))
    //   pixels with ach <= 1e-12 keep their original (near-black) values.
    const double ach = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2]));
    if (!(ach > 1e-12)) {  // near-black (or non-finite ach) -> identity, matching np.where
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        return;
    }
    for (int c = 0; c < 3; ++c) {
        const double d = (ach - rgb[c]) / ach;  // safe_ach == ach here (ach > 1e-12)
        const double dc = reinhard_knee(d, threshold, limit, power);
        out[c] = ach * (1.0 - dc);
    }
}

void compress_rgb_aces_rgc(double* rgb, int npix, double threshold, double limit,
                           double power) {
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_aces_rgc(in, threshold, limit, power, px);
    }
}

// ---------------------------------------------------------------------------
// Input-side: radial xy compression toward the visible spectral locus.
// ---------------------------------------------------------------------------
namespace {

// CIE 1931 2 deg spectral locus, 380..700 nm @ 5 nm, first vertex repeated (closed).
// Captured from colour-science via gamut_compression.py::spectral_locus_xy()
// (tools/parity/gen_gamut_in_golden.py); 66 vertices / 65 edges. Embedded as a
// constant so the radial compression reproduces the oracle bit-for-bit without
// bundling a CMF table — tests/test_gamut_in_xy.cpp re-verifies it against the oracle.
constexpr int kSpectralLocusVerts = 66;
constexpr double kSpectralLocusXy[kSpectralLocusVerts][2] = {
    {0.1741122344263416, 0.00496372598145272},
    {0.17400791751588918, 0.004980548622995036},
    {0.17380077262082788, 0.004915411905373405},
    {0.1735599065272137, 0.004923202577307893},
    {0.17333686548078078, 0.0047967434472668885},
    {0.17302096545549497, 0.004775050361859285},
    {0.17257655084880216, 0.004799301919720766},
    {0.1720866307552483, 0.0048325242180399484},
    {0.17140743386310872, 0.005102170973749332},
    {0.17030098877973637, 0.005788504996470994},
    {0.16887752067098927, 0.00690024388793052},
    {0.16689529035208048, 0.00855560636081898},
    {0.16441175637527494, 0.01085755827676388},
    {0.16110457958027466, 0.013793358821732412},
    {0.15664093257730702, 0.01770480499089134},
    {0.15098540837597124, 0.022740193291642986},
    {0.14396039603960398, 0.029702970297029722},
    {0.13550267119961157, 0.0398791214721278},
    {0.12411847672778563, 0.05780251337374045},
    {0.10959432361561011, 0.08684251118309427},
    {0.0912935070022711, 0.13270204248699013},
    {0.06870592129105556, 0.20072321772810214},
    {0.045390734674777715, 0.29497596460628756},
    {0.02345994254707948, 0.4127034790935206},
    {0.008168028004667443, 0.5384230705117518},
    {0.0038585209003215433, 0.6548231511254019},
    {0.013870246085011192, 0.750186428038777},
    {0.03885180240320428, 0.8120160213618158},
    {0.07430242477337495, 0.833803091340228},
    {0.11416071960667964, 0.8262069597811889},
    {0.15472206121571344, 0.8058635454256492},
    {0.1928760978777212, 0.781629216363077},
    {0.22961967264964017, 0.7543290899027438},
    {0.2657750849711837, 0.7243239249298064},
    {0.3016037993957512, 0.6923077623715741},
    {0.3373633328508564, 0.6588482901396886},
    {0.37310154386845756, 0.624450859796661},
    {0.40873625570642336, 0.5896068688595312},
    {0.44406246358233303, 0.5547139028085305},
    {0.47877479115758376, 0.5202023072114564},
    {0.5124863667817968, 0.48659078806085704},
    {0.5447865055948337, 0.45443411456883603},
    {0.5751513113651648, 0.42423223492490464},
    {0.6029327855757162, 0.3964966335729773},
    {0.6270365997638726, 0.37249114521841786},
    {0.6482331060136394, 0.35139491630502157},
    {0.6657635762380971, 0.33401065115476053},
    {0.680078849721707, 0.31974721706864556},
    {0.6915039729617021, 0.30834226055665565},
    {0.7006060606060607, 0.29930069930069925},
    {0.7079177916216642, 0.2920271089348396},
    {0.7140315971169937, 0.2859288735456499},
    {0.7190329416297438, 0.280934951518654},
    {0.7230316025730948, 0.27694835774834164},
    {0.7259923175416133, 0.2740076824583867},
    {0.7282717282717283, 0.27172827172827163},
    {0.7299690128375388, 0.27003098716246127},
    {0.7310893955845097, 0.2689106044154904},
    {0.7319932998324957, 0.26800670016750433},
    {0.7327188940092165, 0.2672811059907835},
    {0.7334169672259683, 0.2665830327740317},
    {0.7340473003123604, 0.2659526996876395},
    {0.7343901649951473, 0.2656098350048527},
    {0.7345916616426285, 0.2654083383573716},
    {0.7346900232582807, 0.2653099767417192},
    {0.1741122344263416, 0.00496372598145272},
};

// gamut_compression.py::_ray_polygon_distance: distance from `origin` along `dir` to
// the first intersection with the closed locus polygon, via parametric segment
// intersection. denom = dir.x*ey - dir.y*ex; valid iff |denom| > 1e-12; accept the
// hit iff t > 1e-9 and 0 <= s <= 1. Returns +inf on a miss (numpy t_min init = inf).
double ray_polygon_distance(const double origin[2], const double dir[2]) {
    double t_min = std::numeric_limits<double>::infinity();
    for (int k = 0; k < kSpectralLocusVerts - 1; ++k) {
        const double ax = kSpectralLocusXy[k][0], ay = kSpectralLocusXy[k][1];
        const double ex = kSpectralLocusXy[k + 1][0] - ax;
        const double ey = kSpectralLocusXy[k + 1][1] - ay;
        const double denom = dir[0] * ey - dir[1] * ex;
        if (std::fabs(denom) > 1e-12) {
            const double ox = origin[0] - ax, oy = origin[1] - ay;
            const double t = (-ox * ey + oy * ex) / denom;
            const double s = (-ox * dir[1] + oy * dir[0]) / denom;
            if (t > 1e-9 && s >= 0.0 && s <= 1.0 && t < t_min) t_min = t;
        }
    }
    return t_min;
}

}  // namespace

int spectral_locus_xy(const double** out_xy) {
    *out_xy = &kSpectralLocusXy[0][0];
    return kSpectralLocusVerts;
}

void compress_pixel_xy(const double xy[2], const double white_xy[2], double threshold,
                       double limit, double power, double out[2]) {
    // gamut_compression.py::compress_xy_radial, per point.
    //   delta = xy - white; dist = |delta|; dir = delta / fmax(dist, 1e-12)
    //   boundary = ray_polygon_distance(white, dir); d_norm = dist / fmax(boundary, 1e-12)
    //   new_xy = white + dir * (reinhard_knee(d_norm) * boundary)
    //   return where(dist < 1e-9, xy, new_xy)   (at-white passthrough)
    const double dx = xy[0] - white_xy[0];
    const double dy = xy[1] - white_xy[1];
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double safe_dist = std::fmax(dist, 1e-12);
    const double dir[2] = {dx / safe_dist, dy / safe_dist};
    const double boundary = ray_polygon_distance(white_xy, dir);
    const double d_norm = dist / std::fmax(boundary, 1e-12);
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    // 0 * inf == NaN on a genuine ray miss (boundary == inf), as numpy propagates;
    // for an interior white_xy every ray hits, so boundary is finite in practice.
    const double scaled = d_comp * boundary;
    const double nx = white_xy[0] + dir[0] * scaled;
    const double ny = white_xy[1] + dir[1] * scaled;
    if (dist < 1e-9) {  // np.where((dist < 1e-9), xy, new_xy): at-white passthrough
        out[0] = xy[0];
        out[1] = xy[1];
    } else {
        out[0] = nx;
        out[1] = ny;
    }
}

void compress_xy_radial(double* xy, int npix, const double white_xy[2],
                        double threshold, double limit, double power) {
    for (int p = 0; p < npix; ++p) {
        double* q = xy + static_cast<long>(p) * 2;
        const double in[2] = {q[0], q[1]};
        compress_pixel_xy(in, white_xy, threshold, limit, power, q);
    }
}

// ---------------------------------------------------------------------------
// Output-side: OkLch perceptual chroma reduction to the output RGB cube
// (gamut_compression.py::compress_rgb_oklch_chroma + _build_polar_perceptual_c_max_table
// + _c_max_lookup + _compress_lightness, specialised to algorithm=="oklch").
// ---------------------------------------------------------------------------
namespace {

// π as the bit-exact double NumPy uses for np.pi (== M_PI); hex-literal so the h_grid
// endpoints are byte-identical to np.linspace(-np.pi, np.pi, ...) regardless of platform
// math.h ANSI guards.
constexpr double kPi = 0x1.921fb54442d18p+1;

// --- Oklab shared matrices (bit-exact hex, colour 0.4.7). vector_dot(M, v) == M @ v. ---
// M1: XYZ -> LMS ; M2: LMS' -> OkLab. Inverses are colour's MATRIX_1_LMS_TO_XYZ /
// MATRIX_2_LAB_TO_LMS, which equal np.linalg.inv(M1)/np.linalg.inv(M2) bit-for-bit.
constexpr double kOklabM1[3][3] = {
    {  0x1.a34b2ffffd19dp-1,  0x1.728d320078e3dp-2, -0x1.07e79a00e84a6p-3 },
    {  0x1.0e359a0122b3cp-5,  0x1.dbcec3ffc10d5p-1,  0x1.281ae60381493p-5 },
    {  0x1.8adb5bfc6d32ep-5,  0x1.0eb607ffccd61p-2,  0x1.4488360028552p-1 },
};
constexpr double kOklabM2[3][3] = {
    {  0x1.af02a3fe8a4fap-3,  0x1.9655120032aadp-1, -0x1.0add9bd572b38p-8 },
    {  0x1.fa5e1bfffde12p+0, -0x1.36dc1bffe5d3ep+1,  0x1.cd686fff371a5p-2 },
    {  0x1.a869680b729e0p-6,  0x1.90c776001f502p-1, -0x1.9e0ac0001353dp-1 },
};
constexpr double kOklabM1Inv[3][3] = {  // LMS -> XYZ
    {  0x1.3a1d946a3a87fp+0, -0x1.1d97f58537d01p-1,  0x1.20019ca670854p-2 },
    { -0x1.4c6ecd6633e58p-5,  0x1.1cbcddbfc1706p+0, -0x1.259671ec13145p-4 },
    { -0x1.38db94efa7dd7p-4, -0x1.af98f8c4a28d6p-2,  0x1.960ecaf5e947dp+0 },
};
constexpr double kOklabM2Inv[3][3] = {  // OkLab -> LMS'
    {  0x1.fffffff2b0a82p-1,  0x1.95d992fe38807p-2,  0x1.b9f75219cc80fp-3 },
    {  0x1.0000002625996p+0, -0x1.b0611710080c0p-4, -0x1.058bf485b8813p-4 },
    {  0x1.000000ead0f39p+0, -0x1.6e86f739b7095p-4, -0x1.4a9ecbd4621c4p+0 },
};

// --- Per-output-space native RGB<->XYZ (illuminant = the space's OWN whitepoint, NO
// chromatic adaptation to D65 — this is colour.RGB_to_XYZ / XYZ_to_RGB with
// illuminant=cs.whitepoint & cctf off). Indexed by spk_color_space (0..5). Index 5
// (LINEAR_SRGB) is byte-identical to index 0 (sRGB) because cctf is off. Bit-exact hex.
constexpr double kRgbToXyz[6][3][3] = {
    {  // [0] sRGB
        {  0x1.a64c2f837b4a2p-2,  0x1.6e2eb1c432ca5p-2,  0x1.71a9fbe76c8b3p-3 },
        {  0x1.b367a0f9096bcp-3,  0x1.6e2eb1c432ca5p-1,  0x1.27bb2fec56d5dp-4 },
        {  0x1.3c36113404ea5p-6,  0x1.e83e425aee632p-4,  0x1.e6a7ef9db22d1p-1 },
    },
    {  // [1] Adobe RGB (1998)
        {  0x1.27414a4d2b2c0p-1,  0x1.7c06e19b90ea9p-3,  0x1.817ebaf102363p-3 },
        {  0x1.3079e59f2ba9dp-2,  0x1.41355475a31a5p-1,  0x1.3463497b7414ap-4 },
        {  0x1.badc0980b2420p-6,  0x1.218bd66277c46p-4,  0x1.fb90ea9e6eeb7p-1 },
    },
    {  // [2] ProPhoto RGB
        {  0x1.986c226809d49p-1,  0x1.14e3bcd35a857p-3,  0x1.0068db8bac70fp-5  },
        {  0x1.26e978d4fdf3bp-2,  0x1.6c7e28240b780p-1,  0x1.a36e2eb1c4333p-14 },
        { -0x1.529d8bce9dd8dp-60, -0x1.7d26869097f3bp-61, 0x1.a6594af4f0d84p-1  },
    },
    {  // [3] ITU-R BT.2020
        {  0x1.461f5d84c18dbp-1,  0x1.282ce83acff98p-3,  0x1.59de44c9f941ap-3 },
        {  0x1.0d0148ccf66f1p-2,  0x1.5b22902fd967ep-1,  0x1.e5ccb69ab60a3p-5 },
        {  0x1.c3f85a235493dp-55, 0x1.cbf168a39d522p-6,  0x1.0f9cb77c699aep+0 },
    },
    {  // [4] ACES2065-1
        {  0x1.e7b4f2983be02p-1, -0x1.88f09f952796fp-56, 0x1.88eaa17e5206ap-14 },
        {  0x1.6038bdb33fb82p-2,  0x1.74d22fc5e7ec9p-1, -0x1.277474fc3e450p-4  },
        { -0x1.945c48566e5f4p-60, -0x1.2482d70c72d1ep-61, 0x1.02425e0661114p+0  },
    },
    {  // [5] LINEAR_SRGB (== [0])
        {  0x1.a64c2f837b4a2p-2,  0x1.6e2eb1c432ca5p-2,  0x1.71a9fbe76c8b3p-3 },
        {  0x1.b367a0f9096bcp-3,  0x1.6e2eb1c432ca5p-1,  0x1.27bb2fec56d5dp-4 },
        {  0x1.3c36113404ea5p-6,  0x1.e83e425aee632p-4,  0x1.e6a7ef9db22d1p-1 },
    },
};
constexpr double kXyzToRgb[6][3][3] = {
    {  // [0] sRGB
        {  0x1.9ecbfb15b573fp+1, -0x1.8985f06f69446p+0, -0x1.fe90ff9724746p-2 },
        { -0x1.f013a92a30553p-1,  0x1.e0346dc5d6388p+0,  0x1.53f7ced916876p-5 },
        {  0x1.c84b5dcc63f13p-5, -0x1.a1cac083126e9p-3,  0x1.0e978d4fdf3b6p+0 },
    },
    {  // [1] Adobe RGB (1998)
        {  0x1.0552d234eb9a1p+1, -0x1.2148fd9fd36f9p-1, -0x1.6100e6afcce1dp-2 },
        { -0x1.f04039abf3387p-1,  0x1.e03f91e646f15p+0,  0x1.5475a31a4bdbdp-5 },
        {  0x1.b866e43aa79bap-7, -0x1.e4cd74927913fp-4,  0x1.03e22e5de15cap+0 },
    },
    {  // [2] ProPhoto RGB
        {  0x1.589374bc6a7f0p+0, -0x1.05bc01a36e2ecp-2, -0x1.a29c779a6b50fp-5  },
        { -0x1.16d5cfaacd9e8p-1,  0x1.8219652bd3c36p+0,  0x1.4fdf3b645a1cep-6  },
        { -0x1.aab28f12ea173p-60, -0x1.e6fdffe5e01c0p-61, 0x1.36594af4f0d84p+0 },
    },
    {  // [3] ITU-R BT.2020
        {  0x1.b77673c6f9e49p+0, -0x1.6c34f641d9636p-2, -0x1.03727351a2d1bp-2 },
        { -0x1.5557a6bfc0412p-1,  0x1.9dd1b6ddf1d7cp+0,  0x1.025a1324e0e31p-6 },
        {  0x1.2102ecb55b896p-6, -0x1.5e607a2582443p-5,  0x1.e25b571e54eebp-1 },
    },
    {  // [4] ACES2065-1
        {  0x1.0cc06a33249a9p+0, -0x1.1b40fff904389p-55, -0x1.98e12f51c9fb9p-14 },
        { -0x1.fbce0088cee1ap-2,  0x1.5f91719ae1931p+0,  0x1.926424e351582p-4  },
        { -0x1.5ce4fb528d408p-60, -0x1.8e31f5b810833p-61, 0x1.fb85627086a78p-1  },
    },
    {  // [5] LINEAR_SRGB (== [0])
        {  0x1.9ecbfb15b573fp+1, -0x1.8985f06f69446p+0, -0x1.fe90ff9724746p-2 },
        { -0x1.f013a92a30553p-1,  0x1.e0346dc5d6388p+0,  0x1.53f7ced916876p-5 },
        {  0x1.c84b5dcc63f13p-5, -0x1.a1cac083126e9p-3,  0x1.0e978d4fdf3b6p+0 },
    },
};

// C_max table geometry (gamut_compression.py: _OKLCH_CMAX_TABLE_N_L / _N_H / _N_BISECT,
// with the OUTPUT-side L grid linspace(0.02, 1.0, 64) from _get_output_c_max_table).
constexpr int kOklchNL = 64;
constexpr int kOklchNH = 720;
constexpr int kOklchNBisect = 18;

// colour.algebra.spow: sign-preserving power. Negative LMS occur for OOG/negative-XYZ
// pixels, so the cube-root and cube must preserve sign (plain pow would NaN). spow(0)=0.
inline double spow(double a, double p) {
    if (a == 0.0) return 0.0;
    return std::copysign(std::pow(std::fabs(a), p), a);
}

inline void mat3_mul(const double M[3][3], const double v[3], double out[3]) {
    out[0] = M[0][0] * v[0] + M[0][1] * v[1] + M[0][2] * v[2];
    out[1] = M[1][0] * v[0] + M[1][1] * v[1] + M[1][2] * v[2];
    out[2] = M[2][0] * v[0] + M[2][1] * v[1] + M[2][2] * v[2];
}

// colour.XYZ_to_Oklab: LMS = M1 @ XYZ ; LMS' = spow(LMS, 1/3) ; OkLab = M2 @ LMS'.
inline void oklab_from_xyz(const double xyz[3], double lab[3]) {
    double lms[3];
    mat3_mul(kOklabM1, xyz, lms);
    const double lms_[3] = {spow(lms[0], 1.0 / 3.0), spow(lms[1], 1.0 / 3.0),
                            spow(lms[2], 1.0 / 3.0)};
    mat3_mul(kOklabM2, lms_, lab);
}

// colour.Oklab_to_XYZ: LMS' = M2inv @ OkLab ; LMS = spow(LMS', 3) ; XYZ = M1inv @ LMS.
inline void xyz_from_oklab(const double lab[3], double xyz[3]) {
    double lms_[3];
    mat3_mul(kOklabM2Inv, lab, lms_);
    const double lms[3] = {spow(lms_[0], 3.0), spow(lms_[1], 3.0), spow(lms_[2], 3.0)};
    mat3_mul(kOklabM1Inv, lms, xyz);
}

// Build the (kOklchNL x kOklchNH) C_max(L,h) table for output `space`, plus its grids.
// Ports _build_polar_perceptual_c_max_table for space=="oklch": L_grid=linspace(0.02,1,64)
// (inclusive; endpoint pinned to exactly 1.0 as np.linspace does), h_grid=linspace(-π,π,
// 720, endpoint=False), chroma upper 0.5, kOklchNBisect bisection iterations, in-gamut iff
// every native-RGB channel is in [-1e-6, 1+1e-6], returning the lower bracket `lo`.
void build_oklch_cmax_table(int space, double* table, double* L_grid, double* h_grid) {
    // np.linspace(0.02, 1.0, 64): y = arange(64) * step + 0.02 ; y[-1] := 1.0.
    const double Lstep = (1.0 - 0.02) / static_cast<double>(kOklchNL - 1);
    for (int i = 0; i < kOklchNL; ++i)
        L_grid[i] = static_cast<double>(i) * Lstep + 0.02;
    L_grid[kOklchNL - 1] = 1.0;  // np.linspace endpoint override
    // np.linspace(-π, π, 720, endpoint=False): step = 2π/720 ; y = arange(720)*step - π.
    const double hstart = -kPi;
    const double hstep = (kPi - (-kPi)) / static_cast<double>(kOklchNH);
    for (int j = 0; j < kOklchNH; ++j)
        h_grid[j] = static_cast<double>(j) * hstep + hstart;

    const double(*M)[3] = kXyzToRgb[space];
    for (int i = 0; i < kOklchNL; ++i) {
        const double L = L_grid[i];
        for (int j = 0; j < kOklchNH; ++j) {
            // np.cos/np.sin(h_mesh) are constant across the bisection; hoist them.
            const double ch = std::cos(h_grid[j]);
            const double sh = std::sin(h_grid[j]);
            double lo = 0.0;
            double hi = 0.5;
            for (int it = 0; it < kOklchNBisect; ++it) {
                const double mid = (lo + hi) * 0.5;
                const double lab[3] = {L, mid * ch, mid * sh};
                double xyz[3];
                xyz_from_oklab(lab, xyz);
                double rgb[3];
                mat3_mul(M, xyz, rgb);
                const bool in_gamut =
                    rgb[0] >= -1e-6 && rgb[0] <= 1.0 + 1e-6 &&
                    rgb[1] >= -1e-6 && rgb[1] <= 1.0 + 1e-6 &&
                    rgb[2] >= -1e-6 && rgb[2] <= 1.0 + 1e-6;
                if (in_gamut) lo = mid; else hi = mid;
            }
            table[static_cast<size_t>(i) * kOklchNH + j] = lo;
        }
    }
}

// Bilinear C_max lookup with hue wrap (gamut_compression.py::_c_max_lookup). L is clipped
// to the grid; h wraps mod kOklchNH (index 719 <-> 0). h_step and the L denominator are
// taken from the stored grid values (not the analytic step) to match the oracle exactly.
double cmax_lookup(double L, double h, const double* table, const double* L_grid,
                   const double* h_grid) {
    if (L < L_grid[0]) L = L_grid[0];
    else if (L > L_grid[kOklchNL - 1]) L = L_grid[kOklchNL - 1];

    const double h_step = h_grid[1] - h_grid[0];
    const double h_idx = (h - h_grid[0]) / h_step;
    const double h_floor = std::floor(h_idx);
    int h_lo = static_cast<int>(h_floor);
    h_lo = ((h_lo % kOklchNH) + kOklchNH) % kOklchNH;  // numpy floored modulo
    const int h_hi = (h_lo + 1) % kOklchNH;
    const double h_frac = h_idx - h_floor;

    const double L_idx = (L - L_grid[0]) / (L_grid[kOklchNL - 1] - L_grid[0]) *
                         static_cast<double>(kOklchNL - 1);
    int L_lo = static_cast<int>(std::floor(L_idx));
    if (L_lo < 0) L_lo = 0;
    else if (L_lo > kOklchNL - 2) L_lo = kOklchNL - 2;
    const int L_hi = L_lo + 1;
    const double L_frac = L_idx - static_cast<double>(L_lo);

    const double v00 = table[static_cast<size_t>(L_lo) * kOklchNH + h_lo];
    const double v01 = table[static_cast<size_t>(L_lo) * kOklchNH + h_hi];
    const double v10 = table[static_cast<size_t>(L_hi) * kOklchNH + h_lo];
    const double v11 = table[static_cast<size_t>(L_hi) * kOklchNH + h_hi];
    const double t0 = v00 * (1.0 - L_frac) * (1.0 - h_frac);
    const double t1 = v01 * (1.0 - L_frac) * h_frac;
    const double t2 = v10 * L_frac * (1.0 - h_frac);
    const double t3 = v11 * L_frac * h_frac;
    return t0 + t1 + t2 + t3;
}

// One linear-RGB triple -> compressed triple, given a prebuilt C_max table + grids for
// `space`. Mirrors compress_rgb_oklch_chroma per pixel (with lightness_compression pinned
// to (0.7,1.0,2.2), L_white=1). `out` may alias `rgb_in`.
void compress_pixel_oklch(const double rgb_in[3], int space, const double* table,
                          const double* L_grid, const double* h_grid, double threshold,
                          double limit, double power, double out[3]) {
    double xyz[3];
    mat3_mul(kRgbToXyz[space], rgb_in, xyz);
    double lab[3];
    oklab_from_xyz(xyz, lab);
    const double a = lab[1];
    const double b = lab[2];

    // _compress_lightness(L, (0.7,1.0,2.2), L_white=1.0): L/1 and *1 are exact no-ops,
    // so this reduces to the one-sided reinhard knee on L. C_max lookup + reconstruction
    // use this COMPRESSED L; C and h stay on the ORIGINAL a,b.
    const double L = reinhard_knee(lab[0], 0.7, 1.0, 2.2);
    const double C = std::hypot(a, b);
    const double h = std::atan2(b, a);

    const double C_max = cmax_lookup(L, h, table, L_grid, h_grid);
    const double safe_C_max = std::fmax(C_max, 1e-9);
    const double d_norm = C / safe_C_max;
    const double d_comp = reinhard_knee(d_norm, threshold, limit, power);
    const double C_new = d_comp * safe_C_max;

    const double a_new = C_new * std::cos(h);
    const double b_new = C_new * std::sin(h);
    const double lab_new[3] = {L, a_new, b_new};
    double xyz_new[3];
    xyz_from_oklab(lab_new, xyz_new);
    mat3_mul(kXyzToRgb[space], xyz_new, out);
}

}  // namespace

void compress_rgb_oklch_chroma(double* rgb, int npix, int output_space, double threshold,
                               double limit, double power) {
    // Build the per-space C_max(L,h) table ONCE, locally (no static/global state ->
    // thread-invariant and warm==cold). One build per image; this path is opt-in.
    std::vector<double> table(static_cast<size_t>(kOklchNL) * kOklchNH);
    double L_grid[kOklchNL];
    double h_grid[kOklchNH];
    build_oklch_cmax_table(output_space, table.data(), L_grid, h_grid);
    for (int p = 0; p < npix; ++p) {
        double* px = rgb + static_cast<long>(p) * 3;
        const double in[3] = {px[0], px[1], px[2]};
        compress_pixel_oklch(in, output_space, table.data(), L_grid, h_grid, threshold,
                             limit, power, px);
    }
}

}  // namespace spk
