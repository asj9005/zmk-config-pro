"""Compile complete Prospector overlays against deterministic queue/LVGL fakes.

These regressions cover state coalescing and queue selection, not Zephyr
concurrency, ARM timing, LVGL rendering fidelity, or hardware behavior.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
OPERATOR = ROOT / "compat/prospector-zmk-module/boards/shields/prospector_adapter/src/layouts/operator"
FIXTURES = ROOT / "tests/fixtures/prospector"


class ProspectorRuntimeTests(unittest.TestCase):
    def test_actual_overlays_and_negative_controls(self):
        compiler_name = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if not compiler_name:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A C compiler is required for Prospector runtime tests")
            self.skipTest("Set CC or put GCC/Clang on PATH")
        compiler = Path(shutil.which(compiler_name) or compiler_name).resolve()
        env = os.environ.copy()
        env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
        stubs = (FIXTURES / "host-stubs.h").read_text(encoding="utf-8")
        listener = (FIXTURES / "pinned-display-listener.h").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="prospector-") as directory:
            work = Path(directory)

            def run(name, source, expect_pass=True):
                # Retain the complete implementation, including init and its
                # real listener macro. Only the external API headers are faked.
                source = re.sub(r"^#include[^\n]*", "", source, flags=re.M)
                constants = sorted(set(re.findall(r"\b(?:DISPLAY_COLOR_|LV_PART_|LV_ALIGN_|LV_OPA_|LV_FLEX_|LV_OBJ_FLAG_|LV_ANIM_)[A-Z0-9_]+", source)))
                defines = "\n".join(f"#define {constant} {index}" for index, constant in enumerate(constants, 1))
                fixture = (FIXTURES / (name.split("-", 1)[0] + "-main.c")).read_text(encoding="utf-8")
                unit = stubs + "\n" + listener + "\n" + defines + "\n" + source + "\n" + fixture
                path = work / (name + ".c")
                path.write_text(unit, encoding="utf-8")
                executable = work / (name + ".exe")
                build = subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                        "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                                        str(path), "-o", str(executable)], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=10)
                if expect_pass:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout.strip())
                else:
                    self.assertNotEqual(result.returncode, 0, "Defect mutation escaped: " + name)

            battery = (OPERATOR / "battery_circles.c").read_text(encoding="utf-8")
            wpm = (OPERATOR / "wpm_meter.c").read_text(encoding="utf-8")
            output = (OPERATOR / "output.c").read_text(encoding="utf-8")
            run("battery", battery)
            run("wpm", wpm)
            run("output", output)

            mutations = {
                "battery-forget-other-source": (battery, "static struct battery_circles_state state;", "struct battery_circles_state state = {0};"),
                "battery-redraw-duplicates": (battery, "if (peripheral_battery[source] == state.level[source] &&", "if (false && peripheral_battery[source] == state.level[source] &&"),
                "battery-init-before-ready": (battery, "widget->initialized = true;", "widget_battery_circles_init();\n    widget->initialized = true;"),
                "wpm-system-queue": (wpm, "k_work_schedule_for_queue(zmk_display_work_q(), &wpm_smooth_work, K_MSEC(33))", "k_work_schedule(&wpm_smooth_work, K_MSEC(33))"),
                "wpm-init-after-schedule": (wpm, "k_work_init_delayable(&wpm_smooth_work, wpm_smooth_work_handler);", "/* missing work initialization */"),
                "output-direct-render": (output, "return zmk_endpoint_get_selected().transport;", "output_update_cb(zmk_endpoint_get_selected().transport);\n    return zmk_endpoint_get_selected().transport;"),
            }
            for name, (source, before, after) in mutations.items():
                self.assertIn(before, source, name)
                mutated = source.replace(before, after, 1)
                if name == "battery-init-before-ready":
                    # Reproduce the upstream order with no second init after
                    # the widget joins the list.
                    mutated = mutated.replace("sys_slist_append(&widgets, &widget->node);\n    widget_battery_circles_init();", "sys_slist_append(&widgets, &widget->node);")
                run(name, mutated, expect_pass=False)
            print("Prospector: 3 complete overlays and 6 defect mutations checked")


if __name__ == "__main__":
    unittest.main()
