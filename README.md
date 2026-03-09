# Audio Recording Plugin for OBS

This plugin adds an `Audio Recording Filter` to OBS audio sources.
It records each filtered source to its own WAV file.

## What It Does

- Records audio from the source the filter is attached to
- Writes PCM WAV files (`.wav`)
- Supports three trigger modes:
  - `Filter Active`
  - `OBS Stream Active`
  - `OBS Recording Active`
- Includes gain control for recorded output (`-60 dB` to `+24 dB`)
- Shows live filter status in properties (`Idle`, `Active`, `Error`)

## Notes

- `Filter Active` creates a new file each time the filter is toggled on
- `OBS Stream Active` and `OBS Recording Active` follow OBS state while the filter is enabled
- Files are saved using the configured output path and filename pattern

## Build (Windows)

From the repository root:

```powershell
cmake --preset windows-local-x64
cmake --build --preset windows-local-x64 --config RelWithDebInfo
```

## Other Platforms

- macOS build support is included in the template/presets, but currently untested for this plugin.
- Linux build support is included in the template/presets, but currently untested for this plugin.

## Package Output (Windows)

Release artifacts are packaged like this:

```text
audio-recording-plugin/
  bin/64bit/
    audio-recording-plugin.dll
    audio-recording-plugin.pdb
  data/locale/
    en-US.ini
```

The installer places the plugin in:

```text
C:\ProgramData\obs-studio\plugins
```

## License

GPL-2.0-or-later (see `LICENSE`).
