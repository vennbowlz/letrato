// glyphs.cpp - template stroke definitions for A-Z and 0-9.
//
// Stroke order and direction follow common primary-school handwriting
// practice (top-to-bottom, left-to-right, curves starting at the top).
// Adjust them here to match a specific curriculum; the scorer uses whatever
// order is defined, so directionality is graded against these paths.
#include "glyphs.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace {

constexpr float kPi = 3.14159265358979f;

// Small builder: S(x, y) starts a stroke, .L() draws a line to a point,
// .A() sweeps an elliptical arc (angles in degrees, 0 = right, 90 = down).
struct S {
    Stroke pts;
    S(float x, float y) { pts.push_back({x, y}); }
    S& L(float x, float y) { pts.push_back({x, y}); return *this; }
    S& A(float cx, float cy, float rx, float ry, float a0, float a1) {
        int steps = std::max(4, (int)(std::fabs(a1 - a0) / 6.0f));
        for (int i = 0; i <= steps; ++i) {
            float a = (a0 + (a1 - a0) * i / steps) * kPi / 180.0f;
            pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
        }
        return *this;
    }
    operator Stroke() const { return pts; }
};

// Arc-only stroke, starting on the arc itself.
S arc(float cx, float cy, float rx, float ry, float a0, float a1) {
    float a = a0 * kPi / 180.0f;
    S s(cx + rx * std::cos(a), cy + ry * std::sin(a));
    s.A(cx, cy, rx, ry, a0, a1);
    return s;
}

std::map<char, Glyph> buildGlyphs() {
    std::map<char, Glyph> g;

    // ---------------- letters ----------------
    g['A'] = {S(.50f, 0).L(.15f, 1), S(.50f, 0).L(.85f, 1), S(.30f, .62f).L(.70f, .62f)};
    g['B'] = {S(.22f, 0).L(.22f, 1),
              S(.22f, 0).L(.50f, 0).A(.50f, .25f, .25f, .25f, -90, 90).L(.22f, .50f),
              S(.22f, .50f).L(.52f, .50f).A(.52f, .75f, .28f, .25f, -90, 90).L(.22f, 1)};
    g['C'] = {arc(.55f, .50f, .38f, .50f, -40, -320)};
    g['D'] = {S(.22f, 0).L(.22f, 1),
              S(.22f, 0).L(.40f, 0).A(.40f, .50f, .40f, .50f, -90, 90).L(.22f, 1)};
    g['E'] = {S(.25f, 0).L(.25f, 1), S(.25f, 0).L(.78f, 0), S(.25f, .50f).L(.68f, .50f),
              S(.25f, 1).L(.78f, 1)};
    g['F'] = {S(.25f, 0).L(.25f, 1), S(.25f, 0).L(.78f, 0), S(.25f, .50f).L(.68f, .50f)};
    g['G'] = {arc(.55f, .50f, .40f, .50f, -40, -360).L(.60f, .50f)};
    g['H'] = {S(.20f, 0).L(.20f, 1), S(.80f, 0).L(.80f, 1), S(.20f, .50f).L(.80f, .50f)};
    g['I'] = {S(.50f, 0).L(.50f, 1), S(.30f, 0).L(.70f, 0), S(.30f, 1).L(.70f, 1)};
    g['J'] = {S(.70f, 0).L(.70f, .70f).A(.45f, .70f, .25f, .30f, 0, 180)};
    g['K'] = {S(.22f, 0).L(.22f, 1), S(.80f, 0).L(.22f, .58f).L(.82f, 1)};
    g['L'] = {S(.25f, 0).L(.25f, 1).L(.78f, 1)};
    g['M'] = {S(.12f, 1).L(.12f, 0).L(.50f, .62f).L(.88f, 0).L(.88f, 1)};
    g['N'] = {S(.20f, 1).L(.20f, 0).L(.80f, 1).L(.80f, 0)};
    g['O'] = {arc(.50f, .50f, .40f, .50f, -90, -450)};
    g['P'] = {S(.22f, 0).L(.22f, 1),
              S(.22f, 0).L(.48f, 0).A(.48f, .27f, .30f, .27f, -90, 90).L(.22f, .54f)};
    g['Q'] = {arc(.50f, .50f, .40f, .50f, -90, -450), S(.58f, .70f).L(.90f, 1.02f)};
    g['R'] = {S(.22f, 0).L(.22f, 1),
              S(.22f, 0).L(.48f, 0).A(.48f, .27f, .30f, .27f, -90, 90).L(.22f, .54f),
              S(.45f, .54f).L(.82f, 1)};
    g['S'] = {arc(.50f, .25f, .30f, .25f, -20, -270).A(.50f, .75f, .30f, .25f, -90, 160)};
    g['T'] = {S(.15f, 0).L(.85f, 0), S(.50f, 0).L(.50f, 1)};
    g['U'] = {S(.20f, 0).L(.20f, .65f).A(.50f, .65f, .30f, .35f, 180, 0).L(.80f, 0)};
    g['V'] = {S(.15f, 0).L(.50f, 1).L(.85f, 0)};
    g['W'] = {S(.04f, 0).L(.27f, 1).L(.50f, .35f).L(.73f, 1).L(.96f, 0)};
    g['X'] = {S(.20f, 0).L(.80f, 1), S(.80f, 0).L(.20f, 1)};
    g['Y'] = {S(.18f, 0).L(.50f, .50f), S(.82f, 0).L(.50f, .50f).L(.50f, 1)};
    g['Z'] = {S(.20f, 0).L(.80f, 0).L(.20f, 1).L(.80f, 1)};

    // ---------------- digits ----------------
    g['0'] = {arc(.50f, .50f, .34f, .50f, -90, -450)};
    g['1'] = {S(.30f, .20f).L(.52f, 0).L(.52f, 1)};
    g['2'] = {arc(.50f, .30f, .30f, .30f, -165, 30).L(.20f, 1).L(.80f, 1)};
    g['3'] = {arc(.48f, .25f, .30f, .25f, -160, 90).A(.48f, .75f, .32f, .25f, -90, 160)};
    g['4'] = {S(.62f, 0).L(.14f, .68f).L(.86f, .68f), S(.62f, 0).L(.62f, 1)};
    g['5'] = {S(.76f, 0).L(.30f, 0).L(.26f, .45f).A(.50f, .68f, .30f, .32f, -130, 150)};
    g['6'] = {arc(.56f, .50f, .36f, .50f, -60, -180).A(.50f, .70f, .30f, .30f, 180, -180)};
    g['7'] = {S(.20f, 0).L(.80f, 0).L(.40f, 1)};
    g['8'] = {arc(.50f, .25f, .25f, .25f, -30, -270).A(.50f, .75f, .30f, .25f, -90, 270)
                  .A(.50f, .25f, .25f, .25f, 90, -30)};
    g['9'] = {arc(.50f, .30f, .30f, .30f, 0, -360).L(.76f, 1)};

    // ---------------- label-only characters ----------------
    g['%'] = {S(.80f, 0).L(.20f, 1), arc(.25f, .18f, .12f, .16f, -90, -450),
              arc(.75f, .82f, .12f, .16f, -90, -450)};
    g['!'] = {S(.50f, 0).L(.50f, .68f), arc(.50f, .94f, .04f, .04f, 0, 360)};
    g['-'] = {S(.25f, .50f).L(.75f, .50f)};
    g['+'] = {S(.50f, .22f).L(.50f, .78f), S(.22f, .50f).L(.78f, .50f)};
    g['<'] = {S(.70f, .15f).L(.30f, .50f).L(.70f, .85f)};
    g['>'] = {S(.30f, .15f).L(.70f, .50f).L(.30f, .85f)};
    return g;
}

// Accepted alternative stroke orders (taught order lives in buildGlyphs).
std::map<char, std::vector<Glyph>> buildVariants() {
    std::map<char, std::vector<Glyph>> v;
    // A: one stroke up the left leg and down the right, then the bar
    v['A'] = {{S(.15f, 1).L(.50f, 0).L(.85f, 1), S(.30f, .62f).L(.70f, .62f)}};
    // B: 2 strokes (stem, then both lobes continuous) or 1 continuous stroke (down, up, both lobes)
    v['B'] = {{S(.22f, 0).L(.22f, 1),
               S(.22f, 0).L(.50f, 0).A(.50f, .25f, .25f, .25f, -90, 90).L(.22f, .50f)
                   .L(.52f, .50f).A(.52f, .75f, .28f, .25f, -90, 90).L(.22f, 1)},
              {S(.22f, 0).L(.22f, 1).L(.22f, 0).L(.50f, 0).A(.50f, .25f, .25f, .25f, -90, 90).L(.22f, .50f)
                   .L(.52f, .50f).A(.52f, .75f, .28f, .25f, -90, 90).L(.22f, 1)}};
    // D: 1 continuous stroke (down, up, lobe)
    v['D'] = {{S(.22f, 0).L(.22f, 1).L(.22f, 0).L(.40f, 0).A(.40f, .50f, .40f, .50f, -90, 90).L(.22f, 1)}};
    // K: 3 strokes (stem, upper diagonal in, lower diagonal out)
    v['K'] = {{S(.22f, 0).L(.22f, 1), S(.80f, 0).L(.22f, .58f), S(.22f, .58f).L(.82f, 1)}};
    // M: left side down first, then the middle and right side, or 3 separate strokes
    v['M'] = {{S(.12f, 0).L(.12f, 1), S(.12f, 0).L(.50f, .62f).L(.88f, 0).L(.88f, 1)},
              {S(.12f, 0).L(.12f, 1), S(.12f, 0).L(.50f, .62f).L(.88f, 0), S(.88f, 0).L(.88f, 1)}};
    // N: left side down first, then the diagonal and up the right, or 3 separate strokes
    v['N'] = {{S(.20f, 0).L(.20f, 1), S(.20f, 0).L(.80f, 1).L(.80f, 0)},
              {S(.20f, 0).L(.20f, 1), S(.20f, 0).L(.80f, 1), S(.80f, 1).L(.80f, 0)}};
    // P: 1 continuous stroke (down, up, lobe)
    v['P'] = {{S(.22f, 0).L(.22f, 1).L(.22f, 0).L(.48f, 0).A(.48f, .27f, .30f, .27f, -90, 90).L(.22f, .54f)}};
    // R: 2 strokes (stem+lobe, then leg)
    v['R'] = {{S(.22f, 0).L(.22f, 1).L(.22f, 0).L(.48f, 0).A(.48f, .27f, .30f, .27f, -90, 90).L(.22f, .54f),
               S(.45f, .54f).L(.82f, 1)}};
    // Y: 2-stroke alternatives (both arm-first and stem-continuous orders,
    // straight diagonal styles, V-shape + stem, high/low intersections, and 3-stroke).
    v['Y'] = {
        // Left arm + stem continuous, then right arm
        {S(.18f, 0).L(.50f, .50f).L(.50f, 1), S(.82f, 0).L(.50f, .50f)},
        // Right arm + stem continuous first, then left arm
        {S(.82f, 0).L(.50f, .50f).L(.50f, 1), S(.18f, 0).L(.50f, .50f)},
        // Right arm first, then left arm + stem continuous
        {S(.82f, 0).L(.50f, .50f), S(.18f, 0).L(.50f, .50f).L(.50f, 1)},
        // Two straight lines: long diagonal from top-right to bottom-center, left arm to center
        {S(.82f, 0).L(.50f, 1), S(.18f, 0).L(.50f, .50f)},
        {S(.18f, 0).L(.50f, .50f), S(.82f, 0).L(.50f, 1)},
        // Two straight lines: long diagonal from top-left to bottom-center, right arm to center
        {S(.18f, 0).L(.50f, 1), S(.82f, 0).L(.50f, .50f)},
        {S(.82f, 0).L(.50f, .50f), S(.18f, 0).L(.50f, 1)},
        // Slanted print Y: full diagonal top-right to bottom-left + left arm
        {S(.82f, 0).L(.35f, 1), S(.18f, 0).L(.50f, .50f)},
        {S(.18f, 0).L(.50f, .50f), S(.82f, 0).L(.35f, 1)},
        // Slanted print Y: full diagonal top-left to bottom-right + right arm
        {S(.18f, 0).L(.65f, 1), S(.82f, 0).L(.50f, .50f)},
        // Upper V-shape, then vertical stem (and vice-versa)
        {S(.18f, 0).L(.50f, .50f).L(.82f, 0), S(.50f, .50f).L(.50f, 1)},
        {S(.50f, .50f).L(.50f, 1), S(.18f, 0).L(.50f, .50f).L(.82f, 0)},
        // High intersection (y = 0.42)
        {S(.18f, 0).L(.50f, .42f), S(.82f, 0).L(.50f, .42f).L(.50f, 1)},
        {S(.18f, 0).L(.50f, .42f).L(.50f, 1), S(.82f, 0).L(.50f, .42f)},
        // Low intersection (y = 0.60)
        {S(.18f, 0).L(.50f, .60f), S(.82f, 0).L(.50f, .60f).L(.50f, 1)},
        {S(.18f, 0).L(.50f, .60f).L(.50f, 1), S(.82f, 0).L(.50f, .60f)},
        // 3-stroke variant: left arm, right arm, vertical stem
        {S(.18f, 0).L(.50f, .50f), S(.82f, 0).L(.50f, .50f), S(.50f, .50f).L(.50f, 1)}
    };
    // T and E/F: bar(s) before the down stroke
    v['T'] = {{S(.50f, 0).L(.50f, 1), S(.15f, 0).L(.85f, 0)}};
    v['E'] = {{S(.78f, 0).L(.25f, 0).L(.25f, 1).L(.78f, 1), S(.25f, .50f).L(.68f, .50f)}};
    v['F'] = {{S(.78f, 0).L(.25f, 0).L(.25f, 1), S(.25f, .50f).L(.68f, .50f)}};
    // 1 without the flag
    v['1'] = {{S(.52f, 0).L(.52f, 1)}};
    // 4 as one open stroke: down, across, then the stem
    v['4'] = {{S(.62f, 0).L(.14f, .68f).L(.86f, .68f), S(.62f, .30f).L(.62f, 1)}};
    // 5 as two strokes: down-stem and belly first, then the top horizontal bar
    v['5'] = {{S(.30f, 0).L(.26f, .45f).A(.50f, .68f, .30f, .32f, -130, 150), S(.30f, 0).L(.76f, 0)}};
    // 8 starting at the top-center
    v['8'] = {{arc(.50f, .25f, .25f, .25f, -90, -270).A(.50f, .75f, .30f, .25f, -90, 270)
                  .A(.50f, .25f, .25f, .25f, 90, -90)},
              // two stacked circles ("snowman" 8)
              {arc(.50f, .25f, .25f, .25f, -90, -450), arc(.50f, .75f, .30f, .25f, -90, -450)}};
    return v;
}

const std::map<char, Glyph>& table() {
    static const std::map<char, Glyph> t = buildGlyphs();
    return t;
}

}  // namespace

const std::string kSymbols = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

const Glyph& glyphFor(char c) {
    static const Glyph empty;
    auto it = table().find(c);
    return it == table().end() ? empty : it->second;
}

void glyphExtent(char c, float& minX, float& maxX) {
    const Glyph& g = glyphFor(c);
    if (g.empty()) { minX = 0; maxX = 0.45f; return; }  // space / unknown
    minX = 1e9f; maxX = -1e9f;
    for (const Stroke& s : g)
        for (const Point& p : s) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); }
}

std::vector<Glyph> glyphVariants(char c) {
    static const std::map<char, std::vector<Glyph>> alt = buildVariants();
    std::vector<Glyph> out{glyphFor(c)};
    auto it = alt.find(c);
    if (it != alt.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
}

std::vector<Point> glyphPath(const Glyph& g, float ox, float oy, float size, float margin) {
    std::vector<Point> out;
    float inner = size - 2 * margin;
    for (const Stroke& s : g)
        for (const Point& p : s)
            out.push_back({ox + margin + p.x * inner, oy + margin + p.y * inner});
    return out;
}
