#!/usr/bin/env python3
"""Package built plugins with their complete corresponding source and licenses."""
# SPDX-License-Identifier: AGPL-3.0-only
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import zipfile


def tree_signature(root, names):
    lines = []
    for name in sorted(names):
        path = root / name
        if path.is_file():
            lines.append(name + ':' + hashlib.sha256(path.read_bytes()).hexdigest() + '\n')
    return hashlib.sha256(''.join(lines).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build')
    parser.add_argument('--output', default='package')
    parser.add_argument('--juce', default=None)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build).resolve()
    output = (root / args.output).resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit('Output directory must be empty; choose a fresh --output path.')
    candidates = [Path(args.juce)] if args.juce else [build / '_deps/juce-src', root / 'third_party/JUCE-8.0.15']
    juce = next((p.resolve() for p in candidates if (p / 'LICENSE.md').is_file()), None)
    if juce is None:
        raise SystemExit('Matching JUCE source is required. Use --juce /path/to/JUCE-8.0.15.')
    artifacts = build / 'PocketBalance_artefacts/Release'
    bundles = list((artifacts / 'VST3').glob('*.vst3')) + list((artifacts / 'AU').glob('*.component'))
    if not bundles:
        raise SystemExit('No Release VST3/AU bundles found. Build and test first.')
    source_names = ['CMakeLists.txt'] + [p.relative_to(root).as_posix()
        for folder in ['Source', 'Tests', 'FLStudio'] for p in (root / folder).rglob('*')
        if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    signature = tree_signature(root, source_names)
    version = re.search(r'project\(PocketBalance VERSION ([0-9.]+)', (root / 'CMakeLists.txt').read_text()).group(1)
    for bundle in bundles:
        fmt = 'AU' if bundle.suffix == '.component' else 'VST3'
        manifest_path = build / ('PocketBalance-' + fmt + '-manifest.json')
        if not manifest_path.is_file():
            raise SystemExit('Build manifest missing. Rebuild this source before packaging: ' + str(bundle))
        manifest = json.loads(manifest_path.read_text())
        names = [p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file()]
        if manifest.get('version') != version or manifest.get('source_sha256') != signature:
            raise SystemExit('Built source/version does not match current source. Rebuild before packaging.')
        if manifest.get('bundle_sha256') != tree_signature(bundle, names):
            raise SystemExit('Plugin bundle changed since its successful build. Rebuild before packaging.')
    output.mkdir(parents=True, exist_ok=True)
    for bundle in bundles:
        if bundle.is_dir():
            shutil.copytree(bundle, output / bundle.name)
        else:
            shutil.copy2(bundle, output / bundle.name)
    for name in ['README.md', 'LICENSE', 'THIRD_PARTY.md']:
        shutil.copy2(root / name, output / name)
    if (root / 'FLStudio').is_dir():
        shutil.copytree(root / 'FLStudio', output / 'FLStudio',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copy2(juce / 'LICENSE.md', output / 'JUCE-LICENSE.md')
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
    for name in tracked:
        if name.startswith('docs/') and (root / name).is_file():
            destination = output / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, destination)
    with zipfile.ZipFile(output / 'Pocket-Balance-corresponding-source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in tracked:
            if name and (root / name).is_file():
                archive.write(root / name, 'PocketBalance/' + name)
        for source in sorted(juce.rglob('*')):
            relative = source.relative_to(juce)
            if source.is_file() and '.git' not in relative.parts:
                archive.write(source, 'PocketBalance/third_party/JUCE-8.0.15/' + relative.as_posix())
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root).decode().strip()
    (output / 'BUILD.txt').write_text('Source commit: ' + commit + '\nLicense: AGPL-3.0-only\n'
        'Source: https://github.com/dylanmccavitt/pocket-balance\n'
        'Complete corresponding source: Pocket-Balance-corresponding-source.zip\n')
    print('Packaged plugins, licenses, and corresponding source:', output)


if __name__ == '__main__':
    main()
