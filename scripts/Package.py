#!/usr/bin/env python3
"""Package built plugins with their complete corresponding source and licenses."""
# SPDX-License-Identifier: AGPL-3.0-only
from pathlib import Path
import argparse
import shutil
import subprocess
import zipfile


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
    output.mkdir(parents=True, exist_ok=True)
    for bundle in bundles:
        if bundle.is_dir():
            shutil.copytree(bundle, output / bundle.name)
        else:
            shutil.copy2(bundle, output / bundle.name)
    for name in ['README.md', 'LICENSE', 'THIRD_PARTY.md']:
        shutil.copy2(root / name, output / name)
    shutil.copy2(juce / 'LICENSE.md', output / 'JUCE-LICENSE.md')
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
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
