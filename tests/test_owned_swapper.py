"""Compile actual owned-swapper callbacks using deterministic OS/USB fakes.

The fixture includes pinned modifier reference-count functions and a legacy
negative control. This checks state transitions, not Zephyr event scheduling,
ARM ABI/stack usage, or real USB delivery. No network access is required.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class OwnedSwapperRuntimeTests(unittest.TestCase):
    def test_actual_callbacks_and_negative_controls(self):
        with tempfile.TemporaryDirectory(prefix="owned-swapper-") as directory:
            ROOT = Path(__file__).resolve().parents[1]
            HERE = ROOT / "tests"
            REFERENCE = (HERE / "fixtures/owned_swapper/pinned-reference-functions.c").read_text(encoding="utf-8")
            WORK = Path(directory)

            def function(source, name):
                pattern = r"(?m)^(?:static\s+)?(?:inline\s+)?(?:void|int|bool|struct\s+\w+)\s+" + name + r"\([^;]+?\)\s*\{"
                match = re.search(pattern, source, re.S)
                if not match:
                    raise RuntimeError("Missing function " + name)
                end = match.end()
                depth = 1
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                return source[match.start():end]

            candidate = (ROOT / "src/behavior_owned_swapper.c").read_text(encoding="utf-8")
            actual = candidate.split("/* TESTABLE_BEGIN */", 1)[1].split("/* TESTABLE_END */", 1)[0]
            hid = REFERENCE
            modifier_functions = "\n".join(function(hid, name) for name in
                ["zmk_hid_register_mod", "zmk_hid_unregister_mod",
                 "zmk_hid_register_mods", "zmk_hid_unregister_mods"])
            central = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/central.c").read_text(encoding="utf-8")
            old = REFERENCE
            fixture = (HERE / "fixtures/owned_swapper/host-fixture.c").read_text(encoding="utf-8")
            fixture = fixture.replace("/* PINNED_MODIFIER_FUNCTIONS */", modifier_functions)
            fixture = fixture.replace("/* ACTUAL_CANDIDATE_FUNCTIONS */", actual)
            fixture = fixture.replace("/* ACTUAL_CENTRAL_CLEANUP */", function(central, "release_source_keys"))
            fixture = fixture.replace("/* PINNED_LEGACY_END */", function(old, "trigger_end_behavior"))
            widget_path = "compat/prospector-zmk-module/boards/shields/prospector_adapter/src/layouts/operator/modifier_indicator.c"
            widget = (ROOT / widget_path).read_text(encoding="utf-8")
            assert "ZMK_SUBSCRIPTION(widget_modifier_indicator, zmk_modifiers_state_changed);" in widget
            widget_state = widget[widget.index("struct modifier_indicator_state {"):widget.index("static void set_modifier_color")]
            fixture = fixture.replace("/* ACTUAL_MODIFIER_WIDGET_GETTER */", widget_state + function(widget, "modifier_indicator_get_state"))
            (WORK / "candidate-test.c").write_bytes(fixture.encode())
            compiler_name = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
            if not compiler_name:
                if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                    self.fail("A C compiler is required for owned-swapper runtime tests")
                self.skipTest("Set CC or put GCC/Clang on PATH to run owned-swapper runtime tests")
            compiler = Path(shutil.which(compiler_name) or compiler_name).resolve()
            env = os.environ.copy()
            env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
            command = [str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", "candidate-test.c", "-o", "candidate-test.exe"]
            build = subprocess.run(command, cwd=WORK, env=env, capture_output=True, text=True, timeout=60)
            if build.returncode:
                print(build.stdout + build.stderr)
                raise SystemExit(build.returncode)
            result = subprocess.run([str(WORK / "candidate-test.exe")], env=env,
                                    capture_output=True, text=True, timeout=10)
            if result.returncode:
                print(result.stdout + result.stderr)
            assert result.returncode == 0, "Candidate fixture failed"
            report = {"status": "passed", "output": result.stdout.strip(),
                      "candidate_sha256": hashlib.sha256(candidate.encode()).hexdigest(),
                      "scope": "Actual candidate callbacks/state machine, central cleanup and Prospector modifier getter; pinned modifier count functions; fake queue/timer/endpoint/sticky-key/event/display scheduling. No ARM/USB/Zephyr concurrency guarantee.",
                      "widget_sha256": hashlib.sha256(widget.encode()).hexdigest(),
                      "target_built": False, "hardware_tested": False}

            # The same fixture must reject deliberately reintroduced defects.
            negative = {}
            mutations = {
                "flip_owned_start": fixture.replace(
                    "int err = zmk_hid_register_mods(MOD_LALT);",
                    "int err = (explicit_modifiers & MOD_LALT) ? "
                    "zmk_hid_unregister_mods(MOD_LALT) : zmk_hid_register_mods(MOD_LALT);", 1),
                "missing_source_cancel": fixture.replace(
                    "    totem_owned_swapper_source_reset(source);\n}", "}\n", 1),
                "missing_source_cutoff": fixture.replace(
                    "if (swapper_source_activation_blocked(source, event.timestamp)) {",
                    "if (false && swapper_source_activation_blocked(source, event.timestamp)) {", 1),
                "missing_modifier_notification": fixture.replace(
                    "if (((before ^ after) & MOD_LALT) != 0) {",
                    "if (false && ((before ^ after) & MOD_LALT) != 0) {", 1),
            }
            for name, mutated in mutations.items():
                assert mutated != fixture, name
                src = WORK / (name + ".c")
                src.write_bytes(mutated.encode())
                exe = WORK / (name + ".exe")
                compiled = subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                           "-pedantic", src.name, "-o", exe.name], env=env, cwd=WORK,
                                          capture_output=True, text=True, timeout=60)
                assert compiled.returncode == 0, compiled.stdout + compiled.stderr
                failed = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
                assert failed.returncode == 87, name + " did not fail as expected: " + failed.stdout + failed.stderr
                negative[name] = {"expected_failure": True, "output": failed.stderr.strip()}
            report["negative_controls"] = negative

            variant_mains = {
                "nonsplit": r'''
            int main(void) {
                reset(); press(0); release(0);
                assert(swapper.source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL && swapper.active);
                position(12, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, false);
                assert(swapper.active); /* own physical release must not interrupt latch */
                position(7, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true);
                callback_at(5000); assert(swapper.active && !queued);
                position(7, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, false);
                callback_at(3600000); assert(swapper.active && !queued);
                position(14, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true); no_owned_keys();
                now = 0; press(0); release(0);
                totem_owned_swapper_source_reset(0);
                totem_owned_swapper_source_reset(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
                assert(swapper.active && SWAPPER_REMOTE_SOURCE_COUNT == 0);
                position(14, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true); no_owned_keys();
                puts("non-split LOCAL source and indefinite latch paths passed");
                return 0;
            }
            ''',
                "without_usb": r'''
            int main(void) {
                reset(); selected_transport = ZMK_TRANSPORT_BLE;
                press(0); release(0); fail_endpoint = true;
                position(14, 0, true); no_owned_keys(); assert(swapper.report_pending);
                fail_endpoint = false; callback_at(scheduled_at);
                assert(!swapper.report_pending && !(sent_mods & MOD_LALT));
                puts("USB-disabled owned release/report retry paths passed");
                return 0;
            }
            ''',
                "one_remote_source": r'''
            int main(void) {
                reset(); assert(SWAPPER_REMOTE_SOURCE_COUNT == 1);
                now = 100; totem_owned_swapper_source_reset(0);
                press_at(0, 12, 100); no_owned_keys();
                press_at(1, 12, 101); no_owned_keys();
                press_at(254, 12, 101); no_owned_keys();
                totem_owned_swapper_source_reset(1);
                totem_owned_swapper_source_reset(254);
                totem_owned_swapper_source_reset(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
                now = 101; press(0); release(0); assert(swapper.active);
                totem_owned_swapper_source_reset(0); no_owned_keys();
                press_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
                release_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
                assert(swapper.active); position(14, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true);
                no_owned_keys(); puts("one-source cutoff and LOCAL/invalid boundary paths passed");
                return 0;
            }
            ''',
                "zero_remote_sources": r'''
            int main(void) {
                reset(); assert(SWAPPER_REMOTE_SOURCE_COUNT == 0);
                totem_owned_swapper_source_reset(0);
                totem_owned_swapper_source_reset(254);
                totem_owned_swapper_source_reset(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL);
                press_at(0, 12, 0); no_owned_keys();
                press_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
                release_at(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 12, 0);
                assert(swapper.active); position(14, ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, true);
                no_owned_keys(); puts("zero-source arrays excluded; LOCAL latch and invalid sources passed");
                return 0;
            }
            ''',
            }
            variants = {}
            for name, main in variant_mains.items():
                contents = fixture[:fixture.index("int main(void)")] + main
                if name == "nonsplit":
                    contents = contents.replace("#define CONFIG_ZMK_SPLIT 1", "#define CONFIG_ZMK_SPLIT 0")
                elif name == "without_usb":
                    contents = contents.replace("#define CONFIG_ZMK_USB 1", "#define CONFIG_ZMK_USB 0")
                elif name == "one_remote_source":
                    contents = contents.replace("#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT 2",
                                                "#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT 1")
                elif name == "zero_remote_sources":
                    contents = contents.replace("#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT 2",
                                                "#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT 0")
                src = WORK / (name + ".c")
                src.write_bytes(contents.encode())
                exe = WORK / (name + ".exe")
                built = subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                        "-Wno-unused-function", "-pedantic", src.name, "-o", exe.name],
                                       cwd=WORK, env=env, capture_output=True, text=True, timeout=60)
                assert built.returncode == 0, built.stdout + built.stderr
                run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
                assert run.returncode == 0, run.stdout + run.stderr
                variants[name] = run.stdout.strip()
            report["host_config_variants"] = variants
            print(json.dumps(report, sort_keys=True))



if __name__ == "__main__":
    unittest.main()
