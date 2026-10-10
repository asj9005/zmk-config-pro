#!/usr/bin/env bash
# Prepare the small, public-source development environment. No firmware keys or SDK.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"

if [[ "$(uname -s)" != Linux ]]; then
    echo 'Cloud setup requires Linux; it does not install tools on the local Windows PC.' >&2
    exit 1
fi

packages=()
command -v gcc >/dev/null 2>&1 || packages+=(build-essential)
command -v git >/dev/null 2>&1 || packages+=(git)
if ! command -v python3 >/dev/null 2>&1 ||
   ! python3 -c 'import venv, ensurepip' >/dev/null 2>&1; then
    packages+=(python3 python3-venv)
fi
if (( ${#packages[@]} )); then
    if ! command -v apt-get >/dev/null 2>&1; then
        echo 'Install Python 3.10+, python3-venv, GCC/libc headers and Git, then rerun.' >&2
        exit 1
    fi
    elevate=()
    if (( EUID != 0 )); then
        command -v sudo >/dev/null 2>&1 || {
            echo 'Missing build tools; install them with administrator access first.' >&2
            exit 1
        }
        elevate=(sudo -n)
    fi
    "${elevate[@]}" apt-get update
    "${elevate[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
fi

python3 -c 'import sys; assert sys.version_info >= (3, 10), "Python 3.10+ is required"'
python3 -m venv .venv-cloud
.venv-cloud/bin/python -m pip install --disable-pip-version-check -r tools/requirements-cloud.txt
bash tools/cloud-test.sh
