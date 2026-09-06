"""Exercise revision recording with real Git/West and a copied config folder."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(importlib.util.find_spec('west'), 'Requires the build image west package')
class BuildRevisionTests(unittest.TestCase):
    def test_copied_manifest_inactive_project_and_actual_head(self):
        with tempfile.TemporaryDirectory(prefix='west-revisions-') as directory:
            root = Path(directory)
            config = root / 'config'
            config.mkdir()
            project = root / 'active'
            project.mkdir()
            env = os.environ.copy()
            env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                       GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
            def git(*args):
                return subprocess.check_output(['git', '-C', str(project), *args], env=env,
                                               text=True, stderr=subprocess.STDOUT).strip()
            git('init')
            git('commit', '--allow-empty', '-m', 'manifest revision')
            revision = git('rev-parse', 'HEAD')
            git('commit', '--allow-empty', '-m', 'actual checked out revision')
            head = git('rev-parse', 'HEAD')
            self.assertNotEqual(revision, head)
            (config / 'west.yml').write_text(f'''manifest:
  group-filter: [-unused]
  projects:
    - name: active
      url: https://example.invalid/active
      revision: {revision}
    - name: inactive
      url: https://example.invalid/inactive
      revision: {revision}
      groups: [unused]
  self:
    path: config
''', encoding='utf-8')
            west = [sys.executable, '-m', 'west']
            subprocess.run(west + ['init', '-l', str(config)], cwd=root, env=env,
                           check=True, capture_output=True, text=True)
            self.assertFalse((config / '.git').exists())
            self.assertFalse((root / 'inactive').exists())
            frozen = subprocess.run(west + ['manifest', '--freeze'], cwd=root, env=env,
                                    capture_output=True, text=True)
            self.assertNotEqual(frozen.returncode, 0)
            bad_list = subprocess.run(west + ['list', '-f', '{name} {sha}'], cwd=root, env=env,
                                     capture_output=True, text=True)
            self.assertNotEqual(bad_list.returncode, 0)
            result = subprocess.run([sys.executable, str(ROOT / 'tools/record_build_revisions.py')],
                                    cwd=root, env=env, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout), [{
                'name': 'active', 'revision': revision, 'head': head,
                'url': 'https://example.invalid/active',
            }])


if __name__ == '__main__':
    unittest.main()
