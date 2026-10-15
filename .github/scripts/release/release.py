"""Release validation and collection; standard library only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil

TAG = re.compile(r"v?(\d+\.\d+\.\d+(?:\.\d+)?)(?:-(beta|rc)(?:\.?\d+)?)?\Z")
EXTENSIONS = ('.tar.xz', '.zip', '.exe', '.pkg', '.deb', '.ddeb', '.flatpak')


def version(tag, spec):
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError(f'Invalid release tag: {tag}; expected [v]1.2.3[.4][-betaN|-rcN]')
    if not TAG.fullmatch(str(spec['version'])):
        raise ValueError('Invalid package version in buildspec.json')
    return {'tag': tag, 'version': tag.removeprefix('v'),
            'package_version': spec['version'], 'prerelease': str(bool(match[2])).lower()}


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def collect(root, out, tag, spec, sha):
    info = version(tag, spec)
    manifest = json.loads(Path(__file__).with_name('manifest.json').read_text())
    out.mkdir(parents=True, exist_ok=False)
    seen, found = {}, set()
    for path in sorted(root.rglob('*')):
        if not path.is_file() or not any(path.name.endswith(e) for e in EXTENSIONS):
            continue
        if path.is_symlink() or path.stat().st_size == 0:
            raise ValueError(f'Empty or symlink package: {path}')
        artifact = path.relative_to(root).parts[0]
        platforms = [p for p in manifest if f'-{p}-' in artifact]
        if len(platforms) != 1:
            raise ValueError(f'Unrecognized artifact platform: {artifact}')
        platform = platforms[0]
        prefix = f"{spec['name']}-{spec['version']}-{platform}-{sha[:9]}"
        if not artifact.startswith(prefix):
            raise ValueError(f'Artifact provenance/name mismatch: {artifact}; expected {prefix}')
        ext = next(e for e in EXTENSIONS if path.name.endswith(e))
        debug = ext == '.ddeb' or 'dSYMs' in path.name or 'dbgsym' in path.name
        name = f"{spec['name']}-{info['version']}-{platform}{'-debug' if debug else ''}{ext}"
        if name in seen:
            if digest(path) == seen[name]:
                continue  # Ubuntu uploads debug files in both artifact groups.
            raise ValueError(f'Filename collision: {name}')
        seen[name] = digest(path)
        dest = out / ('debug' if debug else 'packages') / name
        dest.parent.mkdir(exist_ok=True)
        shutil.copyfile(path, dest)
        if not debug:
            found.add((platform, ext))
    missing = [f'{p}{e}' for p, exts in manifest.items() for e in exts if (p, e) not in found]
    if missing:
        raise ValueError('Missing required packages: ' + ', '.join(missing))
    info['source_commit'] = sha
    (out / 'release-metadata.json').write_text(json.dumps(info, indent=2) + '\n')
    assets = sorted(p for p in out.rglob('*') if p.is_file())
    (out / 'SHA256SUMS.txt').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in assets))
    (out / 'NOTES.md').write_text(
        f"Release `{tag}`; package version `{spec['version']}`; source `{sha}`.\n\n"
        "Downloads: Windows x64 ZIP/installer, macOS universal PKG, Ubuntu 24.04/26.04 x86_64 DEB, Flatpak x86_64.\n\n"
        "Debug symbols are optional assets marked `debug`. Download SHA256SUMS.txt and run `sha256sum -c SHA256SUMS.txt` with all assets in one directory.\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['version', 'collect'])
    parser.add_argument('--tag', required=True)
    parser.add_argument('--spec', default='buildspec.json')
    parser.add_argument('--root', type=Path)
    parser.add_argument('--out', type=Path, default=Path('release-assets'))
    parser.add_argument('--sha')
    args = parser.parse_args()
    spec = json.loads(Path(args.spec).read_text())
    if args.command == 'collect':
        collect(args.root, args.out, args.tag, spec, args.sha)
    else:
        info = version(args.tag, spec)
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
                stream.write(''.join(f'{k}={v}\n' for k, v in info.items()))
        print(json.dumps(info))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError) as exc:
        raise SystemExit(f'Release validation failed: {exc}')
