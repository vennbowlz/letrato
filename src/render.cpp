// render.cpp - geometry-based drawing helpers.
#include "render.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

inline SDL_Vertex vtx(float x, float y, Color c) {
    SDL_Vertex v;
    v.position = {x, y};
    v.color = {c.r, c.g, c.b, c.a};
    v.tex_coord = {0, 0};
    return v;
}

void pushDisc(std::vector<SDL_Vertex>& v, float cx, float cy, float rad, Color c) {
    int segs = std::clamp((int)(rad * 1.2f), 10, 32);
    for (int i = 0; i < segs; ++i) {
        float a0 = 2 * kPi * i / segs, a1 = 2 * kPi * (i + 1) / segs;
        v.push_back(vtx(cx, cy, c));
        v.push_back(vtx(cx + rad * std::cos(a0), cy + rad * std::sin(a0), c));
        v.push_back(vtx(cx + rad * std::cos(a1), cy + rad * std::sin(a1), c));
    }
}

void pushQuad(std::vector<SDL_Vertex>& v, float x0, float y0, float x1, float y1, float x2,
              float y2, float x3, float y3, Color c) {
    v.push_back(vtx(x0, y0, c)); v.push_back(vtx(x1, y1, c)); v.push_back(vtx(x2, y2, c));
    v.push_back(vtx(x0, y0, c)); v.push_back(vtx(x2, y2, c)); v.push_back(vtx(x3, y3, c));
}

void pushSegment(std::vector<SDL_Vertex>& v, Point a, Point b, float half, Color c) {
    float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * half, ny = dx / len * half;
    pushQuad(v, a.x + nx, a.y + ny, b.x + nx, b.y + ny, b.x - nx, b.y - ny, a.x - nx, a.y - ny, c);
}

void submit(SDL_Renderer* r, const std::vector<SDL_Vertex>& v) {
    if (!v.empty()) SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

}  // namespace

namespace draw {

void polyline(SDL_Renderer* r, const std::vector<Point>& pts, float width, Color c, float ox,
              float oy) {
    if (pts.empty()) return;
    std::vector<SDL_Vertex> v;
    v.reserve(pts.size() * 40);
    float half = width * 0.5f;
    Point prev = {pts[0].x + ox, pts[0].y + oy};
    pushDisc(v, prev.x, prev.y, half, c);
    for (size_t i = 1; i < pts.size(); ++i) {
        Point p = {pts[i].x + ox, pts[i].y + oy};
        pushSegment(v, prev, p, half, c);
        pushDisc(v, p.x, p.y, half, c);
        prev = p;
    }
    submit(r, v);
}

void disc(SDL_Renderer* r, float cx, float cy, float radius, Color c) {
    std::vector<SDL_Vertex> v;
    pushDisc(v, cx, cy, radius, c);
    submit(r, v);
}

void roundRect(SDL_Renderer* r, const SDL_FRect& q, float rad, Color c) {
    rad = std::min({rad, q.w * 0.5f, q.h * 0.5f});
    std::vector<SDL_Vertex> v;
    float x0 = q.x, y0 = q.y, x1 = q.x + q.w, y1 = q.y + q.h;
    pushQuad(v, x0 + rad, y0, x1 - rad, y0, x1 - rad, y1, x0 + rad, y1, c);
    pushQuad(v, x0, y0 + rad, x0 + rad, y0 + rad, x0 + rad, y1 - rad, x0, y1 - rad, c);
    pushQuad(v, x1 - rad, y0 + rad, x1, y0 + rad, x1, y1 - rad, x1 - rad, y1 - rad, c);
    pushDisc(v, x0 + rad, y0 + rad, rad, c);
    pushDisc(v, x1 - rad, y0 + rad, rad, c);
    pushDisc(v, x0 + rad, y1 - rad, rad, c);
    pushDisc(v, x1 - rad, y1 - rad, rad, c);
    submit(r, v);
}

void star(SDL_Renderer* r, float cx, float cy, float outer, Color c) {
    std::vector<SDL_Vertex> v;
    float inner = outer * 0.45f;
    for (int i = 0; i < 10; ++i) {
        float a0 = -kPi / 2 + kPi * i / 5, a1 = -kPi / 2 + kPi * (i + 1) / 5;
        float r0 = (i % 2) ? inner : outer, r1 = (i % 2) ? outer : inner;
        v.push_back(vtx(cx, cy, c));
        v.push_back(vtx(cx + r0 * std::cos(a0), cy + r0 * std::sin(a0), c));
        v.push_back(vtx(cx + r1 * std::cos(a1), cy + r1 * std::sin(a1), c));
    }
    submit(r, v);
}

void tape(SDL_Renderer* r, float cx, float cy, float w, float h, float deg, Color c) {
    float a = deg * kPi / 180.0f, cs = std::cos(a), sn = std::sin(a);
    auto P = [&](float x, float y) { return Point{cx + x * cs - y * sn, cy + x * sn + y * cs}; };
    Point p0 = P(-w / 2, -h / 2), p1 = P(w / 2, -h / 2), p2 = P(w / 2, h / 2), p3 = P(-w / 2, h / 2);
    std::vector<SDL_Vertex> v;   // one quad, no overlap, so translucency stays even
    pushQuad(v, p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, c);
    submit(r, v);
}

void camera(SDL_Renderer* r, float cx, float cy, float w, Color body, Color lens) {
    float h = w * 0.66f;
    roundRect(r, {cx - w * 0.22f, cy - h * 0.62f, w * 0.40f, h * 0.30f}, w * 0.06f, body);  // top hump
    roundRect(r, {cx - w / 2, cy - h / 2, w, h}, w * 0.14f, body);
    disc(r, cx, cy + h * 0.04f, h * 0.36f, lens);
    disc(r, cx, cy + h * 0.04f, h * 0.24f, body);
    disc(r, cx, cy + h * 0.04f, h * 0.12f, lens);
    disc(r, cx + w * 0.33f, cy - h * 0.26f, w * 0.05f, lens);                              // flash
}

void viewfinder(SDL_Renderer* r, const SDL_FRect& q, float inset, float arm, float width, Color c) {
    float x0 = q.x + inset, y0 = q.y + inset, x1 = q.x + q.w - inset, y1 = q.y + q.h - inset;
    polyline(r, {{x0, y0 + arm}, {x0, y0}, {x0 + arm, y0}}, width, c);
    polyline(r, {{x1 - arm, y0}, {x1, y0}, {x1, y0 + arm}}, width, c);
    polyline(r, {{x0, y1 - arm}, {x0, y1}, {x0 + arm, y1}}, width, c);
    polyline(r, {{x1 - arm, y1}, {x1, y1}, {x1, y1 - arm}}, width, c);
}

void gear(SDL_Renderer* r, float cx, float cy, float radius, Color c) {
    disc(r, cx, cy, radius, c);
    for (int i = 0; i < 8; ++i) {
        float a = i * (kPi / 4.0f);
        float tx = cx + std::cos(a) * radius * 0.96f;
        float ty = cy + std::sin(a) * radius * 0.96f;
        disc(r, tx, ty, radius * 0.32f, c);
    }
    disc(r, cx, cy, radius * 0.44f, {255, 255, 255, 255});
}

float textWidth(const std::string& s, float h) {
    float w = 0, gap = 0.24f * h;
    for (size_t i = 0; i < s.size(); ++i) {
        float mn, mx;
        glyphExtent((char)std::toupper((unsigned char)s[i]), mn, mx);
        w += (mx - mn) * h + (i + 1 < s.size() ? gap : 0);
    }
    return w;
}

float text(SDL_Renderer* r, const std::string& s, float x, float y, float h, Color c,
           float weight) {
    float pen = x, gap = 0.24f * h, width = std::max(2.0f, h * weight);
    for (char ch : s) {
        char upper = (char)std::toupper((unsigned char)ch);
        float mn, mx;
        glyphExtent(upper, mn, mx);
        for (const Stroke& st : glyphFor(upper)) {
            std::vector<Point> pts;
            pts.reserve(st.size());
            for (const Point& p : st) pts.push_back({pen + (p.x - mn) * h, y + p.y * h});
            polyline(r, pts, width, c);
        }
        pen += (mx - mn) * h + gap;
    }
    return pen - x - gap;
}

void textCentered(SDL_Renderer* r, const std::string& s, float cx, float y, float h, Color c,
                  float weight) {
    text(r, s, cx - textWidth(s, h) * 0.5f, y, h, c, weight);
}

}  // namespace draw
