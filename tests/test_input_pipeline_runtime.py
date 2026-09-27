"""Execute patched USB and scan callbacks with deterministic endpoint/queue fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_esb_ack_runtime import braced_definition
from tests.test_esb_firmware import compiler_command

ROOT = Path(__file__).resolve().parents[1]


class InputPipelineTests(unittest.TestCase):
    def test_actual_callbacks(self):
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get('ESB_REQUIRE_C_COMPILER') == '1':
                self.fail('Host C compiler required')
            self.skipTest('Host C compiler required')
        env = os.environ.copy()
        env['PATH'] = str(Path(shutil.which(compiler[0]) or compiler[0]).parent) + os.pathsep + env.get('PATH', '')
        for variant, filename, functions in (
            ('usb', 'usb_hid.c', ('in_ready_cb', 'zmk_usb_hid_set_protocol', 'set_proto_cb',
                                'get_keyboard_report', 'totem_usb_hid_status_changed',
                                'usb_queue_current_reports_locked', 'usb_report_work_cb',
                                'zmk_usb_hid_queue_report', 'zmk_usb_hid_send_keyboard_report',
                                'zmk_usb_hid_send_consumer_report', 'zmk_usb_hid_send_mouse_report')),
            ('scan', 'physical_layouts.c', ('queue_scan_event', 'dispatch_scan_event',
                                          'zmk_physical_layouts_kscan_process_msgq')),
        ):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory(prefix='totem-input-') as directory:
                source = (ROOT / 'compat/zmk/app/src' / filename).read_text(encoding='utf-8')
                callbacks = '\n'.join(braced_definition(source, rf'^[^\n;{{}}]*\b{name}\([^;{{}}]*\)\s*\{{') for name in functions)
                fixture = (ROOT / 'tests' / f'{variant}_pipeline_runtime_fixture.c').read_text(encoding='utf-8')
                if variant == 'scan':
                    fixture = fixture.replace('/* ACTUAL_SCAN_EVENT */', braced_definition(source, r'^struct zmk_kscan_event\s*\{') + ';')
                else:
                    status_source = (ROOT / 'compat/zmk/app/src/usb.c').read_text(encoding='utf-8')
                    status_functions = ('zmk_usb_get_status', 'zmk_usb_get_conn_state',
                                        'zmk_usb_is_hid_ready', 'usb_status_cb')
                    status_callbacks = '\n'.join(braced_definition(status_source, rf'^[^\n;{{}}]*\b{name}\([^;{{}}]*\)\s*\{{') for name in status_functions)
                    fixture = fixture.replace('/* ACTUAL_USB_STATUS */', status_callbacks)
                fixture = fixture.replace('/* ACTUAL_CALLBACKS */', callbacks)
                cases = [('current', fixture, True)]
                if variant == 'usb':
                    mutations = (
                        ('local_dma', 'int err = hid_int_ep_write(hid_dev, in_flight_packet.data, in_flight_packet.length, NULL);',
                         'struct totem_hid_packet local = in_flight_packet;\n    int err = hid_int_ep_write(hid_dev, local.data, local.length, NULL);'),
                        ('abort_sem', 'in_flight = false;\n        /* These aborts',
                         'in_flight = false;\n        /* These aborts'),
                        ('old_format', '        totem_hid_queue_reset(&report_queue);',
                         '        /* Mutation: retain the old protocol queue. */'),
                        ('stale_pop', 'if (generation == report_queue.generation)',
                         'if (generation == report_queue.generation || true)'),
                    )
                    for name, old, new in mutations:
                        self.assertIn(old, fixture)
                        if name == 'abort_sem':
                            start = fixture.index('void totem_usb_hid_status_changed(')
                            end = fixture.index('static void usb_queue_current_reports_locked(', start)
                            region = fixture[start:end].replace('k_sem_give(&hid_sem);', '(void)hid_sem;', 1)
                            mutant = fixture[:start] + region + fixture[end:]
                        elif name == 'old_format':
                            start = fixture.index('void zmk_usb_hid_set_protocol(')
                            end = fixture.index('static void set_proto_cb(', start)
                            region = fixture[start:end].replace(old, new, 1)
                            mutant = fixture[:start] + region + fixture[end:]
                        else:
                            mutant = fixture.replace(old, new, 1)
                        self.assertNotEqual(mutant, fixture)
                        cases.append((name, mutant, False))
                for case, source_text, succeeds in cases:
                    with self.subTest(case=case):
                        path = Path(directory) / f'{case}.c'
                        output = path.with_suffix('.exe' if os.name == 'nt' else '.out')
                        path.write_text(source_text, encoding='utf-8')
                        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(ROOT / 'include'), str(path), '-o', str(output)]
                        if Path(compiler[0]).stem.lower() in ('cl', 'clang-cl'):
                            flags = ['/nologo', '/std:c11', '/W4', '/WX', f'/I{ROOT / "include"}', str(path), f'/Fe:{output}']
                        build = subprocess.run(compiler + flags, env=env, capture_output=True, text=True, timeout=60)
                        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                        run = subprocess.run([str(output)], env=env, capture_output=True, text=True, timeout=10)
                        if succeeds:
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pipeline scenarios passed', run.stdout)
                        else:
                            self.assertNotEqual(run.returncode, 0, f'{case} unexpectedly passed')
                            self.assertIn('assert', run.stderr.lower())
                            self.assertIn('failed', run.stderr.lower())


if __name__ == '__main__':
    unittest.main()
