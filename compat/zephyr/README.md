# Pinned USB driver overlay

`drivers/usb/device/usb_dc_nrfx.c` comes from
[zmkfirmware/zephyr at 9df4b12b5af3438a8b9d7a33780dc3b3b2f516c1](https://github.com/zmkfirmware/zephyr/blob/9df4b12b5af3438a8b9d7a33780dc3b3b2f516c1/drivers/usb/device/usb_dc_nrfx.c).
The original LF file has SHA-256
`7134b8da13a380f19b0426e1f693ec55b134308f681b4994f31b6bfa3c02d535`.
Its original copyright notices and Apache-2.0 license are retained.

The only functional changes are in `usbd_reinit()` and `usb_dc_ep_write()`:

- Serialize overflow recovery with endpoint writes, mark the controller not ready,
  stop DMA, clear endpoint state while the driver is still initialized, and flush
  stale completion events.
- Reinitialize the common driver before notifying `USB_DC_RESET`. The USB core
  disables interface endpoints during that callback, so the common driver must
  already exist. Notify outside `drv_lock` to avoid an application/driver lock
  inversion. Keep `ready` false until the normal power and enumeration sequence.
- Reissue USBDETECTED when the cable remains attached, matching `usb_dc_attach()`.
- Recheck readiness and endpoint enablement after acquiring the write lock. A
  producer may have passed the initial checks immediately before recovery began.

No elapsed-time heuristic frees a DMA buffer. The HID layer receives its existing
reset notification only after the controller has stopped using that buffer.
The endpoint polling interval and normal transfer path are unchanged.

The root CMake file applies this overlay only for `CONFIG_TOTEM_ESB_COMPAT`, checks
the exact Zephyr Git revision, and restores the original file for subsequent
non-ESB configurations in the same west workspace. Builds sharing that workspace
must remain sequential.

`tests/test_input_pipeline_runtime.py` extracts the real recovery/write functions
and real ZMK HID callbacks into `usb_pipeline_runtime_fixture.c`. It checks abort
ordering, a producer waiting for the driver lock, no submission before USB is
configured again, latest keyboard/button release recovery, and negative controls.
The common-driver hardware and host enumeration are deterministic substitutes;
these tests do not establish recovery timing or RF stability on real hardware.
