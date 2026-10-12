# Licensing and dependencies

Pocket Balance's original source is copyright 2026 Pocket Balance contributors.
It is licensed under GNU Affero General Public License version 3 only (AGPL-3.0-only).
See [LICENSE](LICENSE) for the full terms. The software comes without warranty.

## JUCE 8.0.15

JUCE is used under its AGPLv3 option. No commercial JUCE license is asserted.

- Source: https://github.com/juce-framework/JUCE/tree/8.0.15
- Archive SHA-256: `0eafd736286c6e7bb6958d79fce18d42cb654b8e231890d327b7cece379bd156`
- License and bundled dependency notices: https://github.com/juce-framework/JUCE/blob/8.0.15/LICENSE.md

CMake obtains the pinned JUCE source or accepts an existing checkout.
JUCE is excluded from this Git repository. Its original notices remain in the dependency tree.
The included VST3 SDK uses MIT; AudioUnitSDK uses Apache-2.0.
Other bundled components retain the licenses identified by JUCE's LICENSE.md and their own source files.

Binary packages produced by scripts/Package.py include this notice, the full AGPL license,
JUCE's license summary, and a corresponding-source archive containing the project,
build scripts, and the complete matching JUCE source with its notices.

Project source: https://github.com/dylanmccavitt/pocket-balance
