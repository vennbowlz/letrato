/*
 * test_stroke.c - unit tests for the NASM backend.
 *
 * Linux (32-bit):  make test          (assembles stroke.asm as elf32)
 * Windows:         i686-w64-mingw32-gcc tests/test_stroke.c -Isrc -Lbuild -lstroke
 */
#include <math.h>
#include <stdio.h>
#include "../src/stroke_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

static void line(Point* p, int n, float x0, float y0, float x1, float y1) {
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        p[i].x = x0 + (x1 - x0) * t;
        p[i].y = y0 + (y1 - y0) * t;
    }
}

int main(void) {
    Point a[256], b[256], r[STROKE_N], c;

    /* path length */
    line(a, 11, 0, 0, 30, 40);
    CHECK(NEAR(stroke_path_length(a, 11), 50.0, 1e-3), "path length %f", stroke_path_length(a, 11));

    /* centroid */
    stroke_centroid(a, 11, &c);
    CHECK(NEAR(c.x, 15, 1e-4) && NEAR(c.y, 20, 1e-4), "centroid %f %f", c.x, c.y);

    /* resample: uneven input -> 64 evenly spaced points */
    a[0] = (Point){0, 0}; a[1] = (Point){1, 0}; a[2] = (Point){2, 0}; a[3] = (Point){63, 0};
    int k = stroke_resample(a, 4, r);
    CHECK(k == STROKE_N, "resample count %d", k);
    for (int i = 0; i < STROKE_N; i++)
        CHECK(NEAR(r[i].x, i, 1e-3) && NEAR(r[i].y, 0, 1e-4), "resample[%d] = %f,%f", i, r[i].x, r[i].y);

    /* resample of a single repeated point must not divide by zero */
    a[0] = a[1] = a[2] = (Point){5, 5};
    k = stroke_resample(a, 3, r);
    CHECK(k == STROKE_N && r[63].x == 5 && r[63].y == 5, "degenerate resample");

    /* indicative angle: first point left of centroid -> 0 ; above -> +pi/2 (y down) */
    line(a, 3, 0, 0, 10, 0);
    CHECK(NEAR(stroke_indicative_angle(a, 3), 0.0, 1e-5), "angle0 %f", stroke_indicative_angle(a, 3));
    line(a, 3, 0, 0, 0, 10);
    CHECK(NEAR(stroke_indicative_angle(a, 3), M_PI / 2, 1e-5), "angle90 %f", stroke_indicative_angle(a, 3));
    line(a, 3, 10, 0, 0, 0);
    CHECK(NEAR(fabs(stroke_indicative_angle(a, 3)), M_PI, 1e-5), "angle180 %f", stroke_indicative_angle(a, 3));

    /* rotation about centroid, including the theta = pi edge of fptan */
    float angles[] = {0.0f, 0.7f, -1.3f, (float)M_PI / 2, 3.14159265f, -3.14159265f};
    for (int j = 0; j < 6; j++) {
        line(a, 5, 0, 0, 10, 0);
        stroke_rotate(a, 5, angles[j]);
        double ex = 5 + (0 - 5) * cos(angles[j]), ey = 0 + (0 - 5) * sin(angles[j]);
        CHECK(NEAR(a[0].x, ex, 1e-3) && NEAR(a[0].y, ey, 1e-3),
              "rotate %f -> %f,%f expected %f,%f", angles[j], a[0].x, a[0].y, ex, ey);
        stroke_centroid(a, 5, &c);
        CHECK(NEAR(c.x, 5, 1e-3) && NEAR(c.y, 0, 1e-3), "rotate keeps centroid");
    }

    /* score: identical -> 100, shifted -> lower, far -> 0 */
    line(a, STROKE_N, 0, 0, 100, 100);
    line(b, STROKE_N, 0, 0, 100, 100);
    CHECK(NEAR(stroke_score(a, b, STROKE_N), 100, 1e-3), "identical score %f", stroke_score(a, b, STROKE_N));
    line(b, STROKE_N, 10, 0, 110, 100);
    float s = stroke_score(a, b, STROKE_N);
    /* avg 10, box 110x100 -> half diag 74.33 -> 86.55 */
    CHECK(NEAR(s, 100 * (1 - 10 / (0.5 * sqrt(110 * 110 + 100 * 100))), 1e-2), "shift score %f", s);
    line(b, STROKE_N, 100, 100, 0, 0);
    CHECK(stroke_score(a, b, STROKE_N) < 40, "reversed direction should score low: %f", stroke_score(a, b, STROKE_N));

    /* full pipeline: same shape drawn at different speeds -> ~100 */
    line(a, 200, 50, 50, 50, 250);           /* slow: many samples */
    line(b, 7, 50, 50, 50, 250);             /* fast: few samples  */
    s = stroke_evaluate(a, 200, b, 7);
    CHECK(s > 99.0f, "speed-invariant evaluate %f", s);
    line(b, 7, 50, 250, 50, 50);             /* opposite direction */
    s = stroke_evaluate(a, 200, b, 7);
    CHECK(s < 50.0f, "wrong direction evaluate %f", s);
    line(b, 7, 150, 50, 150, 250);           /* off-centre copy */
    s = stroke_evaluate(a, 200, b, 7);
    CHECK(s < 60.0f, "off-centre evaluate %f", s);
    CHECK(stroke_evaluate(a, 1, b, 7) == 0.0f, "too-short input");
    {   /* a 15-degree tilt is inside the rotation tolerance and forgiven */
        double t = 15 * M_PI / 180, h = 100;
        line(b, 50, (float)(50 - h * sin(t)), (float)(150 - h * cos(t)),
                    (float)(50 + h * sin(t)), (float)(150 + h * cos(t)));
        s = stroke_evaluate(a, 200, b, 50);
        CHECK(s > 98.0f, "tilted evaluate %f", s);
    }

    /* sampling ring */
    sample_reset();
    for (int i = 0; i < 10; i++) sample_isr(i, i * 2);
    k = sample_drain(r, 4);
    CHECK(k == 4 && r[3].x == 3 && r[3].y == 6, "drain partial %d", k);
    k = sample_drain(r, 64);
    CHECK(k == 6 && r[0].x == 4 && r[5].y == 18, "drain rest %d", k);
    CHECK(sample_drain(r, 64) == 0, "drain empty");
    for (int i = 0; i < 9000; i++) sample_isr(1, 1);
    CHECK(sample_overflow() == 9000 - 8192, "overflow %d", sample_overflow());
    sample_reset();
    CHECK(sample_drain(r, 64) == 0 && sample_overflow() == 0, "reset");

    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures != 0;
}
