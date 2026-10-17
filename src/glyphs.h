// glyphs.h - pre-stored character template vectors.
//
// Every traceable symbol (A-Z, 0-9) is a list of strokes in the order and
// direction a child is taught to write them. Coordinates live in a unit box
// (0,0) top-left to (1,1) bottom-right, y pointing down like the screen.
//
// The same data serves three purposes:
//   1. the guide overlay drawn on the canvas,
//   2. the reference path handed to the assembly scorer,
//   3. a tiny vector font for every label in the UI (no font library needed).
#pragma once
#include <string>
#include <vector>
#include "stroke_api.h"

using Stroke = std::vector<Point>;
using Glyph  = std::vector<Stroke>;

// The 36 practice symbols, in menu order.
extern const std::string kSymbols;

// Strokes for any character the UI can draw (symbols plus a few label-only
// characters such as '%', '!', '-', '+'). Unknown characters return empty.
const Glyph& glyphFor(char c);

// Every accepted way of writing a symbol: the taught stroke order first,
// then common alternatives (e.g. A as one up-and-down stroke plus the bar).
// The attempt is scored against each and the best match counts; directions
// inside each variant are still fixed, so backwards strokes still lose points.
std::vector<Glyph> glyphVariants(char c);

// Horizontal extent of a glyph in unit coordinates (for text layout).
void glyphExtent(char c, float& minX, float& maxX);

// Concatenate a glyph's strokes into one continuous path mapped into a
// square of side `size` at (ox, oy) with `margin` on each side - the same
// merge the canvas applies to the child's strokes.
std::vector<Point> glyphPath(const Glyph& g, float ox, float oy, float size, float margin);
inline std::vector<Point> glyphPath(char c, float ox, float oy, float size, float margin) {
    return glyphPath(glyphFor(c), ox, oy, size, margin);
}
