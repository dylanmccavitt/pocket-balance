# Version 0.4.0 validation

Source-name synchronization adds 19 metadata parameters after the original 73 audio parameters.
The original parameter IDs, order, defaults, and plugin identity remain unchanged.

## Passed locally

- Mac universal Release build: VST3, AU, standalone, and host tests.
- Both Apple Silicon and Intel binaries are present. Minimum target: macOS 11.0.
- Five CTest suites: DSP, clashes, source identity, source-sync protocol, and host integration.
- 28 Python companion tests with mocked FL modules.
- Source-sync protocol tests with AddressSanitizer and UndefinedBehaviorSanitizer.
- Installed VST3 and AU v0.4.0: pluginval 1.0.4, strictness 5, including all editor checks.

Tests cover exact UTF-8 names, malformed packets, CRC, retries, mapping gaps, tracks through 500,
recording suspension, project recall, and source updates with the editor open or closed.
They verify that all audio parameters and effective Auto preferences remain unchanged by name updates.

## Native CI

[Run 38291463644](https://github.com/dylanmccavitt/pocket-balance/actions/runs/38291463644) passed for source commit `4e002eb`.
Mac universal and Windows x64 both passed the native build, all five CTest suites, all 28 companion tests, and packaging.
Both platform packages were uploaded as run artifacts. Windows DAW playback remains unverified.

## Live FL checks

The first host probe showed that FL's scripting API did not expose non-automatable or meter parameters.
The implementation now uses generic metadata parameters. Writes to metadata outputs are ignored and corrected on the message thread.
The companion does not transmit while FL is recording. Do not automate the reserved `source_sync_*` parameters.

The installed VST3 exposed all metadata names in FL Studio. RESET packets received exact acknowledgments while stopped and playing.
A write to the read-only context parameter was ignored, and FL's cached value was restored.
All original audio parameter values remained unchanged during these live checks. Transport was stopped afterward.

Full source-name transfer, automatic Mixer rename propagation, and save/reopen behavior remain under live verification.

## Limits

The companion requires a dedicated enabled MIDI input and an explicit source mapping.
FL does not expose Wrapper input assignments to scripts. Check and re-link after routing changes or preset copying.
Source names do not prove that audio reaches the corresponding input. Verify each source meter during setup.
These local Mac builds use ad-hoc signatures and are not Apple-notarized.
AU retains the existing program-index and non-main bus-disable warnings.
Compilation and automated checks do not replace listening tests or establish compatibility with every DAW.
