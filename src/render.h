// render.h - small drawing helpers on top of SDL2's renderer.
// Thick strokes, rounded panels, stars and vector text are all built from
// SDL_RenderGeometry triangles, so there are no image or font assets.
#pragma once
#include <SDL.h>
#include <string>
#include <vector>
#include "glyphs.h"

struct Color { Uint8 r, g, b, a = 255; };

namespace draw {

// Polyline with round joints and caps, `width` pixels wide.
void polyline(SDL_Renderer* r, const std::vector<Point>& pts, float width, Color c,
              float ox = 0, float oy = 0);
void disc(SDL_Renderer* r, float cx, float cy, float radius, Color c);
void roundRect(SDL_Renderer* r, const SDL_FRect& rect, float radius, Color c);
void star(SDL_Renderer* r, float cx, float cy, float outer, Color c);

// --- photograph theme pieces ---
// A strip of translucent tape, centred at (cx, cy), tilted by `deg`.
void tape(SDL_Renderer* r, float cx, float cy, float w, float h, float deg, Color c);
// Simple camera pictogram, `w` pixels wide.
void camera(SDL_Renderer* r, float cx, float cy, float w, Color body, Color lens);
// Viewfinder corner brackets just inside `rect`.
void viewfinder(SDL_Renderer* r, const SDL_FRect& rect, float inset, float arm, float width, Color c);
// Gear pictogram for settings button.
void gear(SDL_Renderer* r, float cx, float cy, float radius, Color c);

// Text drawn with the template glyphs as a stroke font.
// `h` is the cap height in pixels; returns the drawn width.
float text(SDL_Renderer* r, const std::string& s, float x, float y, float h, Color c,
           float weight = 0.13f);
float textWidth(const std::string& s, float h);
void textCentered(SDL_Renderer* r, const std::string& s, float cx, float y, float h, Color c,
                  float weight = 0.13f);

}  // namespace draw
