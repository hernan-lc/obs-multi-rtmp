"""Read-only GitHub preflight and downloaded-asset verification."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import quote
from release import digest


def api(endpoint):
    result = subprocess.run(['gh', 'api', endpoint], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'GitHub access failed for {endpoint}. Check token Contents permission, repository and synchronized tag.')
    return json.loads(result.stdout)


def commit(repo, tag):
    obj = api(f'repos/{repo}/git/ref/tags/{quote(tag, safe="")}')['object']
    while obj['type'] == 'tag':
        obj = api(f"repos/{repo}/git/tags/{obj['sha']}")['object']
    if obj['type'] != 'commit':
        raise ValueError('Tag must resolve to a commit')
    return obj['sha']


def resolve():
    repo, tag = os.environ['GITHUB_REPOSITORY'], os.environ['TAG']
    if not re.fullmatch(r'[\w.-]+/[\w.-]+', repo):
        raise ValueError('Invalid repository')
    sha = commit(repo, tag)
    run = os.environ.get('RECOVERY_RUN', '')
    if run:
        if not run.isdigit():
            raise ValueError('Recovery run ID must be numeric')
        data = api(f'repos/{repo}/actions/runs/{run}')
        if (data['conclusion'] != 'success' or data['head_sha'] != sha
                or data['event'] not in ('push', 'workflow_dispatch')
                or data['path'] not in ('.github/workflows/push.yaml', '.github/workflows/release.yaml')):
            raise ValueError('Recovery requires a successful trusted release run at the exact tag commit')
    with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
        stream.write(f'sha={sha}\n')


def preflight():
    target, tag, sha = (os.environ[k] for k in ('TARGET', 'TAG', 'SOURCE_SHA'))
    if not re.fullmatch(r'[\w.-]+/[\w.-]+', target):
        raise ValueError('Invalid target repository')
    if commit(target, tag) != sha:
        raise ValueError('Destination tag differs from source commit. Synchronize the exact tag before publishing.')
    # Check write access before any publication mutation.
    permissions = api(f'repos/{target}').get('permissions', {})
    if not permissions.get('push'):
        raise ValueError('Release credential requires Contents: write on destination')
    result = subprocess.run(['gh', 'api', f'repos/{target}/releases/tags/{quote(tag, safe="")}'], capture_output=True, text=True)
    if result.returncode:
        if '404' not in result.stderr:
            raise ValueError('Cannot inspect existing release')
        return
    release = json.loads(result.stdout)
    check_assets(target, release, require_all=False)


def check_assets(target, release, require_all):
    assets = release['assets']
    local = [p for p in Path('release-assets').rglob('*') if p.is_file() and p.name != 'NOTES.md']
    with tempfile.TemporaryDirectory() as tmp:
        for path in local:
            matches = [a for a in assets if a['name'] == path.name]
            if not matches and not require_all:
                continue
            if len(matches) != 1:
                raise ValueError(f'Missing or duplicate published asset: {path.name}')
            dest = Path(tmp) / path.name
            with dest.open('wb') as stream:
                result = subprocess.run(['gh', 'api', '-H', 'Accept: application/octet-stream',
                                         f"repos/{target}/releases/assets/{matches[0]['id']}"], stdout=stream, stderr=subprocess.PIPE)
            if result.returncode or digest(dest) != digest(path):
                if not require_all and os.environ.get('MODE') == 'replace':
                    continue
                raise ValueError(f'Asset checksum differs: {path.name}; use explicit replace mode to repair')
    if require_all and release['draft']:
        raise ValueError('Release is still draft')


if __name__ == '__main__':
    try:
        if sys.argv[1] == 'resolve':
            resolve()
        elif sys.argv[1] == 'preflight':
            preflight()
        else:
            target = os.environ['TARGET']
            check_assets(target, api(f"repos/{target}/releases/tags/{quote(os.environ['TAG'], safe='')}"), True)
    except (ValueError, KeyError) as exc:
        raise SystemExit(f'Release failed: {exc}')
