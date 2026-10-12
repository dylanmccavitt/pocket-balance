# FL Studio source names

Pocket Balance can copy source names from FL Studio Mixer tracks through its optional **Pocket Balance Source Sync** companion.
The companion changes source labels only. It does not change roles, gains, mute, solo, audio routing, or frequency corrections.

Each plugin input needs an explicit Mixer-track mapping. FL Studio does not expose its Wrapper input assignments through the scripting API.
The companion therefore cannot infer those assignments safely. The saved mapping must match **Wrapper settings > Processing > Connections**.

## Install the companion

1. Locate FL Studio's **User data folder** in **Options > File settings**.
2. Open that folder in Finder or File Explorer.
3. Open `Settings/Hardware` inside the folder.
4. Copy the complete `FLStudio/PocketBalanceSourceSync` folder from this release into `Settings/Hardware`.
5. Confirm the installed folder contains `device_PocketBalanceSourceSync.py`.
6. Restart FL Studio if the new controller type does not appear.

The resulting path ends with:

```text
Settings/Hardware/PocketBalanceSourceSync/device_PocketBalanceSourceSync.py
```

FL Studio runs MIDI scripts through an enabled MIDI input. Use one dedicated input for this companion.
The script consumes that input's MIDI messages. Do not assign it to the keyboard or pads you use to play instruments.
The companion requires a current FL Studio version with MIDI scripting API 29 or later.

## Create the dedicated input on Mac

1. Open **Audio MIDI Setup** from **Applications > Utilities**.
2. Select **Window > Show MIDI Studio**.
3. Double-click **IAC Driver**.
4. Enable **Device is online**.
5. Add a port named **Pocket Balance Sync**.
6. Open FL Studio's **Options > MIDI settings**.
7. Select the new IAC port under **Input**.
8. Enable the input.
9. Set **Controller type** to **Pocket Balance Source Sync**.

No notes need to enter the port. FL Studio calls the companion's idle callback while the input is enabled.
Do not enable MIDI master sync for this port. The companion sends no MIDI output.

Apple documents the built-in driver in [Transfer MIDI information between apps](https://support.apple.com/en-ie/guide/audio-midi-setup/ams1013/mac).

## Create the dedicated input on Windows

An existing, unused MIDI input can host the companion. Otherwise, create a dedicated virtual MIDI input with a compatible virtual-port utility.
The release does not include a virtual MIDI driver.

For example, [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) provides virtual ports on supported Windows systems.
Follow its installation instructions, create **Pocket Balance Sync**, and keep the utility running.
Then enable that input in **Options > MIDI settings** and select **Pocket Balance Source Sync** as its controller type.

Keep your performance keyboard on its current controller type. Assign this companion only to the dedicated input.

## Map each source

1. Save a copy of your FL Studio project.
2. Click **FL source names** in Pocket Balance.
3. Set **Plugin's Mixer track** to the track containing Pocket Balance.
4. Set **Effect slot** to its slot number, counting the top slot as **1**.
5. Enter each source track number in its input's **MIXER #** field.
6. Leave unused input fields blank.
7. Confirm every mapping against **Wrapper settings > Processing > Connections**.
8. Click **Link source names**.
9. Rename one source Mixer track to check the corresponding plugin label.
10. Save the project after confirming the mapping.

Mixer track numbers refer to FL Studio's numbered inserts. They are separate from plugin input numbers and musical roles.
For example, input **Drum 04** can use Mixer track **18**. Inputs **Drum 02** and **Drum 03** can remain unmapped.
A tom retains its existing **Percussion** role when its source name changes.

The mapping is stored with the plugin state. The companion rediscovers loaded instances after project loading.
It checks the stored owner track and effect slot before sending names. Each instance receives its own mapped labels.
The **Follow FL Mixer names** panel accepts Mixer tracks **0–500**. Track **0** is Master.

## Behavior and limits

- Mixer renaming updates mapped source labels after the companion processes the change.
- Name synchronization pauses while FL Studio records. It restarts with fresh names when recording stops.
- Labels retain up to 256 UTF-8 bytes without splitting a character. Leading and trailing spaces are preserved.
- Empty or whitespace-only Mixer names use `Mixer N` as a fallback label. Embedded NUL characters are removed.
- A source must have an active route to the plugin's owner track.
- A missing route stops that source's updates and changes route status. Its previous label remains available.
- Role selection and **Auto role from label** remain unchanged during synchronization.
- The companion periodically restores mapped Mixer names if a linked label is edited manually.
- Click **Unlink names** before using manual source labels.
- Moving the plugin to another Mixer track or effect slot requires updating its owner settings.
- Changing Wrapper input assignments requires updating the saved source mapping.
- Sidechain and normal routes can both be active. Route detection alone does not identify the correct plugin input.
- This companion supports FL Studio. Other hosts retain manual source labels.

A successful name update does not prove that an input carries the correct audio. Play one source at a time and check its input meter.
The companion does not read audio, project files, sample paths, or network services.

## Troubleshooting

If names do not update, check these items in order:

1. Confirm the companion input is enabled in **Options > MIDI settings**.
2. Confirm its controller type is **Pocket Balance Source Sync**.
3. Confirm Pocket Balance supports source-sync metadata parameters.
4. Confirm the owner Mixer track and effect slot match the loaded plugin.
5. Confirm the source mapping matches the Wrapper input assignment.
6. Confirm the source has an active route to the owner track.
7. Stop recording before checking name synchronization.
8. Check **View > Script output** for a script error.

If several companion inputs are enabled, keep only one enabled. Two scripts can compete for the same metadata stream.
Keep the script and plugin from the same release. Older Pocket Balance versions do not contain the required metadata interface.

See Image-Line's [MIDI scripting API](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/midi_scripting.htm),
[MIDI settings](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/envsettings_midi.htm),
and [Wrapper manual](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/plugins/wrapper.htm) for host behavior.

## Developer verification

Run the companion tests from the repository root:

```sh
python3 -m unittest discover -s Tests -p '*SourceSync*.py'
```

The tests use mock FL modules. They cover Unicode framing, CRC, ACK delays, retries, recovery, route removal, explicit mapping gaps,
multiple instances, slot replacement, project loading, bounded scanning, recording suspension, and unchanged audio parameter indices.
Mock tests do not establish live FL Studio transport compatibility. Record live host validation separately.
