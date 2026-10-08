#!/usr/bin/env bash
# Run all host regressions without downloading Zephyr or touching production keys.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
if [[ ! -x .venv-cloud/bin/python ]]; then
    echo 'Run bash tools/cloud-setup.sh first.' >&2
    exit 1
fi
.venv-cloud/bin/python -c 'import west' # Do not silently skip revision-recording tests.
CC="${CC:-gcc}" ESB_REQUIRE_C_COMPILER=1 \
    .venv-cloud/bin/python -B -m unittest discover -s tests -v
git diff --check
