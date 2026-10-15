import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / '.github/scripts/release'
sys.path.insert(0, str(SCRIPTS))
import release
spec = importlib.util.spec_from_file_location('release_github', SCRIPTS / 'github.py')
github = importlib.util.module_from_spec(spec)
spec.loader.exec_module(github)


class ReleaseTests(unittest.TestCase):
    def test_tags(self):
        for tag in ('1.2.3', 'v1.2.3', 'v1.2.3.4', '1.2.3-beta', 'v1.2.3-beta.2', 'v1.2.3-rc1'):
            self.assertEqual(release.version(tag, {'version': '0.7.5.0'})['prerelease'], str('-' in tag).lower())
        for tag in ('v1', 'v1.2.3-bogus', 'v1.2.3-rcjunk', 'v1.2.3/evil', '1.2.3\n'):
            with self.assertRaises(ValueError):
                release.version(tag, {'version': '0.7.5.0'})

    def fixture(self, root):
        manifest = json.loads((SCRIPTS / 'manifest.json').read_text())
        for platform, exts in manifest.items():
            folder = root / f'obs-multi-rtmp-0.7.5.0-{platform}-abcdef123'
            folder.mkdir(parents=True)
            for ext in exts:
                (folder / f'package{ext}').write_bytes(b'package contents')

    def test_collection_and_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, out = Path(tmp) / 'in', Path(tmp) / 'out'
            self.fixture(root)
            release.collect(root, out, 'v0.0.2', {'name': 'obs-multi-rtmp', 'version': '0.7.5.0'}, 'abcdef123456')
            for line in (out / 'SHA256SUMS.txt').read_text().splitlines():
                checksum, name = line.split('  ')
                self.assertEqual(checksum, release.digest(next(out.rglob(name))))
            with self.assertRaises(FileExistsError):
                release.collect(root, out, 'v0.0.2', {'name': 'obs-multi-rtmp', 'version': '0.7.5.0'}, 'abcdef123456')

    def test_missing_empty_collision(self):
        for mode in ('missing', 'empty', 'collision'):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'in'
                self.fixture(root)
                path = next(root.rglob('*.zip'))
                if mode == 'missing':
                    path.unlink()
                elif mode == 'empty':
                    path.write_bytes(b'')
                else:
                    path.with_name('duplicate.zip').write_bytes(b'different')
                with self.assertRaises(ValueError):
                    release.collect(root, Path(tmp) / 'out', 'v0.0.2', {'name': 'obs-multi-rtmp', 'version': '0.7.5.0'}, 'abcdef123456')

    def test_auth_failure(self):
        with patch.object(github.subprocess, 'run') as run:
            run.return_value.returncode = 1
            with self.assertRaisesRegex(ValueError, 'Check token'):
                github.api('repos/example/repo')

    def test_partial_upload(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(github, 'Path') as paths:
            missing = Path(tmp) / 'missing.zip'
            missing.write_bytes(b'contents')
            paths.return_value.rglob.return_value = [missing]
            with self.assertRaisesRegex(ValueError, 'Missing or duplicate'):
                github.check_assets('example/repo', {'assets': [], 'draft': False}, True)

    def test_retry_preserves_matching_and_rejects_conflicting_assets(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(github, 'Path') as paths:
            asset = Path(tmp) / 'package.zip'
            asset.write_bytes(b'package')
            paths.return_value.rglob.return_value = [asset]
            def download(args, **kwargs):
                kwargs['stdout'].write(b'package')
                return type('Result', (), {'returncode': 0})()
            data = {'assets': [{'name': asset.name, 'id': 1}], 'draft': False}
            with patch.object(github.subprocess, 'run', side_effect=download):
                github.check_assets('a/b', data, True)
                github.check_assets('a/b', data, True)
            def corrupt(args, **kwargs):
                kwargs['stdout'].write(b'corrupt')
                return type('Result', (), {'returncode': 0})()
            with patch.object(github.subprocess, 'run', side_effect=corrupt):
                with self.assertRaisesRegex(ValueError, 'checksum differs'):
                    github.check_assets('a/b', data, True)

    def test_recovery_rejects_wrong_commit(self):
        with patch.dict(github.os.environ, {'GITHUB_REPOSITORY': 'a/b', 'TAG': 'v0.0.2', 'RECOVERY_RUN': '123'}), patch.object(github, 'commit', return_value='correct'), patch.object(github, 'api', return_value={'conclusion': 'success', 'head_sha': 'wrong'}):
            with self.assertRaisesRegex(ValueError, 'exact tag commit'):
                github.resolve()


if __name__ == '__main__':
    unittest.main()
