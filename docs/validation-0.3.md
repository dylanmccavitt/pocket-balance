# Version 0.3.0 validation

Date: October 12, 2026.

## Passed

- Universal macOS Release: arm64 and x86_64, minimum target macOS 11.
- CTest: 4/4 suites: DSP, frequency clashes, identity, and host integration.
- pluginval 1.0.4 strictness 5: VST3 and AU, including GUI checks.
- Standalone clash engine: optimized Clang C++17 tests and AddressSanitizer/UndefinedBehaviorSanitizer.
- Detector tests include repeated/sustained overlap, silence, isolated clicks, alternating hits, separated bands, stereo antiphase, and all eight bands at 44.1/48/96 kHz.
- Correction tests cover frequency selectivity, bounded cuts, attack/release, smooth disable/pair changes, and bypass.
- Host tests cover old parameter indices, older-session recall, unsaved preview, Apply/Undo, stale results, editor closure, view changes, and auxiliary input preservation.
- Native panel snapshots reviewed at 1120 x 880 and 1600 x 1100.
- Packaging rejects stale binaries and includes complete corresponding source and licenses.

## Limits

AU pluginval reports the pre-existing program -1 and non-main bus disable warnings.
These are local development builds, without Apple notarization.
Universal compilation is not a separate Intel/Rosetta runtime test.
Functional tests are not a real-time CPU benchmark or proof of perceptual mix quality.
Windows CI and live FL Studio 0.3 checks will be recorded after they finish.
