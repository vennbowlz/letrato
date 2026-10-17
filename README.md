# LetraTo — starter code

A desktop handwriting-tracing tool for A–Z and 0–9 that teaches mouse control.
The name joins *letra* (letter) and *litrato* (Filipino for "photograph"): every traced letter is a
photo the child takes and keeps in an album.

## Theme

- **Album (menu):** each symbol is a taped-down polaroid. Once a symbol is scored,
  the polaroid shows the child's own best tracing with its stars.
- **Viewfinder (canvas):** the drawing area is a polaroid with camera corner brackets;
  numbered badges beside each start show the stroke order.
- **SNAP (check):** a shutter sound and a white flash, then the photo *develops*
  (fades up from sepia) on the score page, captioned "LETTER A" / "NUMBER 7".
- Retro film-print pastels on a warm album page; motion is limited to the flash
  and the develop fade so it stays non-distracting.

Two layers, as specified:

| Layer | Language | Responsibility |
|---|---|---|
| Frontend | C++17 + SDL2 | Window, symbol menu, canvas, guide overlay (adjustable opacity), clear/check tools, path combination, scoring page, audio cues |
| Backend | 32-bit NASM, x87 FPU | Interrupt-style coordinate sampling, centroid, 64-point resampling, indicative angle + rotation (`fpatan`, `fptan`), distance score |

The backend is a pure-assembly `stroke.dll` (no C runtime). The frontend calls it with
`__cdecl`, passing raw `Point*` + count — no translation layer.

```
asm/stroke.asm      x87 backend (all exported routines)
asm/stroke.def      DLL export list
src/stroke_api.h    C interface to the DLL
src/main.cpp        app state, screens, sampling hook, path combination
src/glyphs.cpp      template stroke vectors (order + direction per symbol)
src/render.cpp      thick strokes / panels / vector text via SDL_RenderGeometry
src/audio.cpp       synthesised chimes (no asset files)
tests/test_stroke.c backend unit tests
```

## Build (32-bit Windows)

**MSYS2, MINGW32 shell**
```
pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2 nasm make
make
```
Run `build/LetraTo.exe` (keep `stroke.dll` and `SDL2.dll` beside it).

**Cross-compile from Linux**
```
make CROSS=i686-w64-mingw32- SDL2_DIR=/path/to/SDL2-2.30.x/i686-w64-mingw32
```

**Backend tests:** `make test` (Linux, gcc-multilib) or `make test-win` (builds `test_stroke.exe`).

## Pipeline

1. **Sampling.** An SDL event watch forwards each mouse report, as SDL pumps OS input, to
   `sample_isr` — an O(1) assembly routine writing into a fixed 8192-point ring. The
   frontend drains it every frame into the current stroke.
2. **Path combination.** There is no timer: every stroke drawn on the canvas belongs to the
   attempt until the child presses SNAP (or Enter/Space). The strokes are then concatenated
   and passed to `stroke_evaluate`, once per accepted stroke order of the symbol
   (`glyphVariants()` in `src/glyphs.cpp`, e.g. A as one up-and-down stroke plus the bar).
   The best match is the score; directions inside each variant stay fixed.
3. **`stroke_evaluate`** (assembly): resample candidate and template to N = 64 → indicative
   angle of each → rotate candidate about its centroid → average point distance →
   `100 × (1 − avg / half-diagonal of the shared bounding box)`, clamped to 0–100.

Off-centre drawing lowers the score because rotation keeps each centroid in place and the
template sits at a fixed position on the canvas.

## Two design decisions to review

- **"Interrupt routines" on Windows.** User-mode Win32 programs cannot install hardware or
  INT 33h mouse handlers (that's DOS-era). The closest faithful equivalent is used here:
  `sample_isr` is constant-time, allocation-free, and is invoked synchronously from the
  input pump through an SDL event watch rather than from the render loop. Worth phrasing
  this way in the paper.
- **Rotation is limited to ±22.5°.** Classic $1 rotates both paths to angle 0, which makes a
  letter drawn *backwards* score 100% — that defeats the "stroke directionality" objective.
  The candidate is therefore rotated toward the template's indicative angle, with the
  correction clamped to π/8 (`k_max_tilt` in `stroke.asm`). Slight tilt is forgiven;
  reversed or wrong-start strokes are not. The unit tests cover both cases.

## Tuning points

- `kInkWidth`, `kGuideWidth` — `src/main.cpp`
- Star thresholds (85 / 65 / 40) — `starsFor()` in `src/main.cpp`
- Stroke order and direction per symbol, and accepted alternatives — `src/glyphs.cpp`
- Rotation tolerance — `k_max_tilt` in `asm/stroke.asm`
