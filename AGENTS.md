# Totem firmware development

This is the public source repository for Totem left/right halves and a Prospector
USB dongle. Read `docs/cloud-development.md` for the cloud workflow.

- Cloud setup: `bash tools/cloud-setup.sh`. Regression checks:
  `bash tools/cloud-test.sh`. The setup installs a small Python environment and
  runs the same host checks; it does not fetch the full firmware SDK.
- Keep production ESB keys, keyconf files, generated private headers/configs, ELF
  and UF2 files out of this repository, cloud development environments, logs and
  public artifacts. Public CI uses disposable test keys only. Production builds
  belong in the separate private builder; do not rotate existing device keys as
  part of ordinary development.
- Keep dependency SHAs in `config/west.yml` and the pinned build image/actions
  unless dependency changes are part of the task. `compat/` overlays modify
  shared dependency checkouts: never run different firmware configurations in
  parallel in the same west workspace. Separate Actions jobs may use separate
  workspaces.
- Run relevant C runtime/protocol tests for behavior changes. Before handing a
  firmware revision to the private builder, run the full host suite and check
  public firmware CI for the exact GitHub commit. Record that full commit SHA,
  rather than only a mutable branch name. Keep build verification distinct from
  hardware verification; host tests cannot establish RF stability or latency.
- Preserve the current keymap/layer behavior and security settings unless the
  task asks to change them. Report changes and remaining hardware checks clearly.
