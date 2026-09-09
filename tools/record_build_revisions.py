#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Record active dependency HEADs; the copied config is not a Git project."""

import json
import re
import subprocess
import sys


def record_revisions():
    # These fields come from the manifest without consulting Git. west list
    # omits inactive projects by default; unlike --freeze it need not clone them.
    listing = subprocess.check_output(
        [sys.executable, '-m', 'west', 'list', '-f', '{name}\t{revision}\t{abspath}\t{url}'],
        text=True,
    )
    records = []
    for line in listing.splitlines():
        name, revision, path, url = line.split('\t', 3)
        if name == 'manifest':
            # CI copies config/ into its west workspace, so this entry has no
            # .git. Its source is already identified by the Actions checkout.
            continue
        commit = subprocess.check_output(
            ['git', '-C', path, 'rev-parse', '--verify', 'HEAD'], text=True,
        ).strip()
        if not re.fullmatch(r'[0-9a-f]{40}', commit):
            raise ValueError(f'Invalid dependency HEAD: {name}')
        records.append({'name': name, 'revision': revision, 'head': commit, 'url': url})
    if not records:
        raise ValueError('No active dependency projects found')
    return records


if __name__ == '__main__':
    print(json.dumps(record_revisions(), indent=2))
