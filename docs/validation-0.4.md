# Version 0.4.0 validation

Source-name synchronization adds 19 metadata parameters after the original 73 audio parameters.
The original parameter IDs, order, defaults, and plugin identity remain unchanged.

## Passed locally

- Mac universal Release build: VST3, AU, standalone, and host tests.
- Both Apple Silicon and Intel binaries are present. Minimum target: macOS 11.0.
- Five CTest suites: DSP, clashes, source identity, source-sync protocol, and host integration.
- 28 Python companion tests with mocked FL modules.
- Source-sync protocol tests with AddressSanitizer and UndefinedBehaviorSanitizer.
- VST3 pluginval 1.0.4, strictness 5, with GUI tests skipped.

Tests cover exact UTF-8 names, malformed packets, CRC, retries, mapping gaps, tracks through 500,
recording suspension, project recall, and source updates with the editor open or closed.
They verify that all audio parameters and effective Auto preferences remain unchanged by name updates.

## Live FL checks

The first host probe showed that FL's scripting API did not expose non-automatable or meter parameters.
The implementation now uses generic metadata parameters. Writes to metadata outputs are ignored and corrected on the message thread.
The companion does not transmit while FL is recording. Do not automate the reserved `source_sync_*` parameters.

Full source-name transfer, automatic Mixer rename propagation, and save/reopen behavior remain under live verification.
Windows native build and DAW runtime results must be recorded separately.

## Limits

The companion requires a dedicated enabled MIDI input and an explicit source mapping.
FL does not expose Wrapper input assignments to scripts. Check and re-link after routing changes or preset copying.
Source names do not prove that audio reaches the corresponding input. Verify each source meter during setup.
These local Mac builds use ad-hoc signatures and are not Apple-notarized.
Compilation and automated checks do not replace listening tests or establish compatibility with every DAW.
