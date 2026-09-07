"""Compile the actual curve helper; this does not model RF, USB, or key timing.

The old-curve oracle keeps the pinned ZMK speed() math after integer-ms conversion.
The new two-stage quadratic is checked against a separate double-precision formula.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <totem/pointing_curve.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "curve check failed at line %d: %s\n", __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

/* Original speed() arithmetic at pinned ZMK 904c9aec, with ticks already
 * converted to integer milliseconds by the behavior's caller. */
static float old_curve(float maximum, uint32_t elapsed, uint16_t duration, uint8_t exponent) {
    if (elapsed > duration || duration == 0 || exponent == 0) {
        return maximum;
    }
    if (elapsed == 0) {
        return 0;
    }
    float fraction = (float)elapsed / duration;
#if defined(CONFIG_MINIMAL_LIBC) && CONFIG_MINIMAL_LIBC
    float power = 1.0f;
    for (float e = (float)exponent; e >= 1.0f; e--) {
        power = power * fraction;
    }
    return maximum * power;
#else
    return maximum * powf(fraction, exponent);
#endif
}

static void same_bits(float actual, float expected) {
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
}

static void near_value(float actual, double expected) {
    CHECK(isfinite(actual));
    CHECK(fabs((double)actual - expected) <= 0.001 + fabs(expected) * 0.000001);
}

static void default_equivalence(void) {
    const float maxima[] = {-3200, -1600, -15, -0.0f, 0, 15, 1600, 3200};
    const uint16_t durations[] = {0, 1, 2, 300, 490, UINT16_MAX};
    const uint8_t exponents[] = {0, 1, 2, 3, 8, UINT8_MAX};
    const uint32_t times[] = {0, 1, 2, 15, 16, 199, 200, 201, 299, 300, 489,
                              490, 491, 65534, 65535, 65536, UINT32_MAX};
    for (size_t m = 0; m < sizeof(maxima) / sizeof(*maxima); m++) {
        for (size_t d = 0; d < sizeof(durations) / sizeof(*durations); d++) {
            for (size_t e = 0; e < sizeof(exponents) / sizeof(*exponents); e++) {
                for (size_t t = 0; t < sizeof(times) / sizeof(*times); t++) {
                    float expected = old_curve(maxima[m], times[t], durations[d], exponents[e]);
                    same_bits(totem_pointing_curve_speed(maxima[m], times[t], durations[d],
                                                        exponents[e], 0, 1, 1), expected);
                }
            }
        }
    }
}

static void boosted_curve(void) {
    float previous = 0;
    for (uint32_t t = 0; t <= 2000; t++) {
        float actual = totem_pointing_curve_speed(1600, t, 490, 2, 200, 7, 4);
        float negative = totem_pointing_curve_speed(-1600, t, 490, 2, 200, 7, 4);
        double base_fraction = (double)t / 490;
        double boost_fraction = t > 200 ? (double)(t - 200) / 290 : 0;
        double expected = t >= 490 ? 2800 :
            1600 * base_fraction * base_fraction + 1200 * boost_fraction * boost_fraction;
        near_value(actual, expected);
        near_value(negative, -expected);
        CHECK(actual >= previous && actual <= 2800);
        CHECK(negative <= 0 && negative >= -2800);
        if (t <= 200) {
            same_bits(actual, old_curve(1600, t, 490, 2));
            same_bits(negative, old_curve(-1600, t, 490, 2));
        }
        if (t >= 490) {
            same_bits(actual, 2800);
            same_bits(negative, -2800);
        }
        previous = actual;
    }
    /* At the join there is no added displacement speed; just after it the
     * original curve plus a small positive quadratic contribution is used. */
    same_bits(totem_pointing_curve_speed(1600, 200, 490, 2, 200, 7, 4),
              old_curve(1600, 200, 490, 2));
    CHECK(totem_pointing_curve_speed(1600, 201, 490, 2, 200, 7, 4) >
          old_curve(1600, 201, 490, 2));
    same_bits(totem_pointing_curve_speed(1600, UINT32_MAX, 490, 2, 200, 7, 4), 2800);
}

static void disabled_and_boundary_modes(void) {
    const uint16_t configs[][3] = {{200, 1, 1}, {200, 7, 7}, {490, 7, 4},
                                  {491, 7, 4}, {200, 7, 0}, {200, 0, 4}, {200, 3, 4}};
    for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
        for (uint32_t t = 0; t <= 600; t++) {
            same_bits(totem_pointing_curve_speed(-1600, t, 490, 2, configs[c][0],
                                                configs[c][1], configs[c][2]),
                      old_curve(-1600, t, 490, 2));
        }
    }
    for (uint32_t t = 0; t <= 600; t++) {
        same_bits(totem_pointing_curve_speed(-15, t, 0, 2, 0, 7, 4), -15);
        same_bits(totem_pointing_curve_speed(15, t, 490, 0, 200, 7, 4), 15);
        same_bits(totem_pointing_curve_speed(0, t, 490, 2, 200, 7, 4), 0);
    }
    /* Zero-start boost is continuous at zero; a one-ms late segment and
     * uint16 extremes do not divide by zero or wrap the elapsed subtraction. */
    same_bits(totem_pointing_curve_speed(1600, 0, 490, 2, 0, 7, 4), 0);
    near_value(totem_pointing_curve_speed(1600, 100, 490, 2, 0, 7, 4),
               2800.0 * (100.0 / 490) * (100.0 / 490));
    same_bits(totem_pointing_curve_speed(1600, 489, 490, 2, 489, 7, 4),
              old_curve(1600, 489, 490, 2));
    same_bits(totem_pointing_curve_speed(1600, 490, 490, 2, 489, 7, 4), 2800);
    same_bits(totem_pointing_curve_speed(1, UINT32_MAX, UINT16_MAX, UINT8_MAX,
                                        UINT16_MAX - 1, UINT16_MAX, 1), (float)UINT16_MAX);
}

int main(void) {
    default_equivalence();
    boosted_curve();
    disabled_and_boundary_modes();
    puts("pointing curve: old equivalence, early exactness, signed formula, monotonicity, cap and boundaries passed");
    return 0;
}
'''


class PointingCurveRuntimeTest(unittest.TestCase):
    def test_actual_helper(self):
        compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
        if not compiler and Path(r"C:\QMK_MSYS\mingw64\bin\gcc.exe").is_file():
            compiler = r"C:\QMK_MSYS\mingw64\bin\gcc.exe"
        if not compiler:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A C compiler is required for the pointing curve test")
            self.skipTest("A C compiler is required for the pointing curve test")
        environment = os.environ.copy()
        environment["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="pointing-curve-") as directory:
            workdir = Path(directory)
            source = workdir / "curve.c"
            source.write_text(FIXTURE, encoding="utf-8")
            for minimal_libc in (0, 1):
                with self.subTest(minimal_libc=minimal_libc):
                    executable = workdir / (f"curve-{minimal_libc}" + (".exe" if os.name == "nt" else ""))
                    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                                    f"-DCONFIG_MINIMAL_LIBC={minimal_libc}", "-I", str(ROOT / "include"),
                                    str(source), "-o", str(executable), "-lm"],
                                   check=True, env=environment, timeout=60)
                    result = subprocess.run([str(executable)], check=False, capture_output=True,
                                            text=True, env=environment, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("old equivalence, early exactness, signed formula, monotonicity, cap and boundaries passed",
                                  result.stdout)


if __name__ == "__main__":
    unittest.main()
