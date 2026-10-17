/*
 * stroke_api.h - C interface to the NASM backend (stroke.dll).
 *
 * Every function uses the native C calling convention (__cdecl). Point
 * arrays are passed as raw pointers with no translation layer: the memory
 * the frontend fills is the memory the assembly reads.
 */
#ifndef STROKE_API_H
#define STROKE_API_H

#if defined(_WIN32)
  #define STROKE_CALL __cdecl
  #if defined(STROKE_STATIC)
    #define STROKE_IMPORT
  #else
    #define STROKE_IMPORT __declspec(dllimport)
  #endif
#else
  #define STROKE_CALL __attribute__((cdecl))
  #define STROKE_IMPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Must match the 8-byte layout the assembly expects. */
typedef struct Point { float x; float y; } Point;

#define STROKE_N 64   /* resampled points per path */

/* ---- interrupt-style sampling ring (O(1) per report) ---- */
STROKE_IMPORT int   STROKE_CALL sample_isr(int x, int y);
STROKE_IMPORT int   STROKE_CALL sample_drain(Point* out, int max);
STROKE_IMPORT void  STROKE_CALL sample_reset(void);
STROKE_IMPORT int   STROKE_CALL sample_overflow(void);

/* ---- x87 recognizer pipeline ---- */
STROKE_IMPORT float STROKE_CALL stroke_path_length(const Point* p, int n);
STROKE_IMPORT void  STROKE_CALL stroke_centroid(const Point* p, int n, Point* out);
STROKE_IMPORT int   STROKE_CALL stroke_resample(const Point* in, int n, Point* out);
STROKE_IMPORT float STROKE_CALL stroke_indicative_angle(const Point* p, int n);
STROKE_IMPORT void  STROKE_CALL stroke_rotate(Point* p, int n, float theta);
STROKE_IMPORT float STROKE_CALL stroke_score(const Point* cand, const Point* tmpl, int n);
STROKE_IMPORT float STROKE_CALL stroke_evaluate(const Point* raw, int n,
                                                const Point* tmplRaw, int m);

#ifdef __cplusplus
}
#endif

#endif
