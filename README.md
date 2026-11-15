# CardputerZero-Cartoon

A camera QR audio player for the 320x170 CardputerZero, based on the
[CardputerZero LVGL template](https://github.com/CardputerZero/Template).

## Interaction

- Launch **CardputerZero-Cartoon** into an idle, fixed two-column screen.
  The left 160x120 camera tile is gray whenever scanning is inactive.
  The right side always shows up to three recent audio tracks, newest first,
  the selected track highlighted, playback status, elapsed time and progress.
- **8 / OK / Enter**: stop audio and start a scan. During camera startup or
  active scanning, press again to end recognition and return to the idle gray
  tile without clearing history or releasing the warm camera.
- A grayscale preview runs at up to 10 fps; QR decoding runs at up to 5 fps.
  The full 320x240 grayscale camera frame is decoded, not just the guide box.
- The preview is rotated 180 degrees to correct the mounted camera orientation.
  QR decoding still receives the unmodified camera frame.
- On a valid audio QR, decoding pauses and audio starts automatically.
  The closest QR code to the scan region center wins when several are visible.
- **6**: pause/resume scanning or audio; retry the current operation on errors.
  From idle with history (or after audio ends), play the selected track from the
  start; with no history, stay idle. Pausing scanning preserves its paused state,
  unlike ending the scan with OK/8. Both discard queued results.
- **5 / 7** (also **Left / Right**): previous/next track in the visible history, immediately
  stopping the current operation and playing the chosen track from the start.
  History does not wrap at the ends.
- **R**: replay the selected track from the start.
- **System volume keys**: use the launcher's existing volume/mute controls.
  The app does not change the system mixer or bind Up/Down to volume. Playback
  uses 100% player gain rather than a second app-level volume multiplier, so
  perceived loudness may be higher than the previous 60% app default.
- **4** (also **S**): scanner settings (see below).
- **ESC**: exit and release both camera and player.

The five bottom hints align with the physical 4-8 keys rather than equal-width
screen columns. Each shows its action above a vertical `|` alignment mark, not
a numeric key label. Scan/stop and play/pause/retry hints follow the current state.
Replay, system volume and exit hints stay in the header; no independent app
volume percentage is shown.
After recognition or scan pause, the camera tile is gray and QR decoding/preview
conversion stop, but capture stays warm until app exit, with no inactivity timeout.
Frames are drained and discarded to avoid blocking the camera pipe. A new scan
reuses capture, discards the in-flight old frame, and displays the first fresh
preview before decoding it. Changing decoder, decode rate or scan region does
not restart capture; changing capture size/exposure or recovering from a camera
failure requires a cold start. Exit releases the camera and player.
Warm standby still occupies the camera and consumes power and memory.

Audio sources must be an absolute Linux file path (for example
`/home/pi/audio/story.mp3`) or an HTTP/HTTPS direct audio URL. Unicode file names
and URL query strings are preserved. Relative paths use the configured audio
base directory (see below); other schemes, multiline
QR text and URLs containing embedded credentials are rejected explicitly.
Ordinary HTML pages are not resolved to audio; there is no browser or shell
execution of QR content. mpv handles supported audio formats with video disabled.

### Relative audio addresses

The settings page displays the audio base and scrolls the actual Linux config
filename. Edit its `base_address=` line on Linux, then reopen settings (or restart
the app) to reload it. There is no on-device free-text editor for this field;
Left/Right on the base row leaves it unchanged. Pressing OK saves other settings
without losing the base. If the file does not exist yet, save settings once.

For the normal `pi` user the default file is
`/home/pi/.config/cardputerzero-cartoon/scanner.conf`; `$XDG_CONFIG_HOME` overrides
that location, and the UI always displays the effective filename.
For example:

```text
320 240 5 0 0 0
base_address=https://example.com/audio/
```

Keep the first line (scanner parameters) and edit the second line only.
Alternatively use `base_address=/home/pi/audio/`. Leave it empty to require
absolute QR addresses. A base must be an absolute Linux directory or an HTTP(S)
directory URL without credentials, query or fragment; it is always treated as
a directory, even without a trailing slash. Invalid settings are reported explicitly.

Address classification:

- `http://...` / `https://...`: direct absolute URL, independent of the base.
- Any leading `/`: local absolute path, never a website-root-relative URL.
- `a.mp3`, `stories/a.mp3`, `./a.mp3`, `../a.mp3`: resolve against the base;
  literal `.` / `..` path segments are normalized. The base is not a sandbox:
  `..` can access its parent directory. Network query/fragment suffixes and
  percent escapes are preserved, not filesystem-normalized.
- Other protocol prefixes (including `file:` and Windows drive paths) are rejected.
- A relative QR with no base gives an actionable settings error.

History stores resolved absolute addresses: changing the base only affects new
scans and never changes the playback targets of the existing three tracks.

The three-item history lives only in memory. Duplicate scans move the existing
source to the top; playback alone never reorders it. A syntactically valid but
unplayable source stays selected for retry. Playback ends without looping or
restarting the camera. Unknown duration is shown as `--:--`, not a fake progress.

## Scanner settings

Press **4** for the separate settings page. Entering it pauses scanning/audio;
returning does not automatically resume either. **Up/Down** selects a row,
**Left/Right** changes it, **OK** saves and returns, and **ESC** cancels changes.
The overlapping F/X/Z/C letter keys remain equivalent alternatives.
New values apply on the next scan:

- Capture resolution: 320x240 (default), 480x360, 640x480, 800x600, or 1280x960.
  Preview remains 160x120; higher capture sizes cost more memory and decoding time.
- QR decode rate: up to 2, 5 (default), or 10 scans per second; camera preview
  remains up to 10 fps, subject to device performance.
- Exposure compensation: -2 to +2 EV, default 0 (not optical focus).
- Decode region: full frame (default) or the central 50% width and height.
  The guide outlines the actual region; preview orientation remains 180 degrees.
- QR algorithm: ZBar (existing default) or ZXing-C++ 2.3. Both read the same
  grayscale scan region, decode only QR codes, and select the nearest recognized
  code to the region center. ZXing uses its built-in harder/rotation/inversion
  search with plain UTF-8 text output; there is no automatic engine fallback or
  extra image enhancement, and no guarantee that either handles all blurred images.

Settings persist atomically in `$XDG_CONFIG_HOME/cardputerzero-cartoon/scanner.conf`
or `$HOME/.config/cardputerzero-cartoon/scanner.conf`. Missing settings use defaults;
unreadable or corrupt settings report an error and can be replaced through **4**.
Existing five-field settings load with ZBar; new saves append the decoder choice
as a sixth field (0 = ZBar, 1 = ZXing-C++), followed by an optional
`base_address=` line. Older five/six-field files retain an empty base.
No autofocus control is exposed because this IMX219 has no usable focus actuator.

## Privacy

Frames and history stay in device memory. The app does not save camera images,
record video, or persist scanned audio addresses. Playing network audio contacts
the QR URL's server and can follow its redirects; local sources do not require
network access. No application-level audio download/cache is retained.
The player runs with user scripts, user configuration and external media
references disabled. App diagnostic logs report errors/state, not scanned paths
or query tokens. Only scanner tuning parameters are saved.

## Hardware and runtime

The initial target is the IMX219 camera on CardputerZero with Debian 13 ARM64.
Capture uses the device's existing `/usr/bin/rpicam-vid` and libcamera.
Audio uses `/usr/bin/mpv` with private Unix-socket JSON IPC and the user's
PulseAudio/PipeWire output. Active QR decoding and playback do not run
simultaneously; warm camera capture can coexist with playback until app exit.
The screen uses `/dev/fb0`; keys use the template's evdev input routing.
Run as the normal device user with `video` and `input` group membership,
preferably through APPLaunch, not as root.

The UI uses the system Noto CJK font for multilingual results. The launcher
uses an original camera-and-QR icon; its editable source is `assets/images/cartoon.svg`.
This initial release targets the Linux device, not the desktop simulator.

## Cloud build

Pushes to `main` and manual workflow dispatches run the ARM64 Debian 13 build:

1. Install build dependencies.
2. Configure and compile the app and focused tests.
3. Run QR, history, settings and scan-region tests, plus real mpv local/HTTP
   playback, pause/resume, replay and end-of-file tests with a null audio sink.
4. Package a `.deb` and upload `cardputerzero-cartoon-arm64`.

No local compiler or SDK is needed.

## Native Linux build

On Debian 13 ARM64:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config git \
    libfmt-dev libpng-dev libjpeg-dev libfreetype-dev zlib1g-dev \
    libzbar-dev libzxing-dev libqrencode-dev libcjson-dev dpkg-dev
cmake --preset cp0-native -DCM0_SDK_ROOT=/ -DCM0_ALLOW_FETCH_DEPS=OFF
cmake --build --preset cp0-native-rel --parallel 1
ctest --test-dir build/cp0-native -C Release --output-on-failure
cpack --config build/cp0-native/CPackConfig.cmake -C Release -G DEB
```

Low-memory devices should use a single build job; cloud builds are recommended.
For builds without tests, set `-DBUILD_TESTING=OFF` and omit `libqrencode-dev`.

Install the generated package with `sudo apt install ./dist/<package>.deb`.
The package installs `/usr/bin/cardputerzero_cartoon` and its own launcher entry,
assets, and icon. It does not replace TemplateApp or install an autostart service.
Removing it uses `sudo apt remove cardputerzero-cartoon`.

## Tests

Generated QR fixtures cover exact URL, Unicode and long-text decoding, blank
frames, unsupported UTF-8/control data, and center selection. Interaction tests
cover idle launch, scan/audio pause, stale-result suppression, autoplay, three-item
deduplicated history, bounded previous/next, replay, retry, scan ending and
selected-track playback from idle.
Settings tests cover persistence and malformed input; region tests verify
center-only cropping and fixed preview scaling for the capture resolutions. An asymmetric four-corner
frame checks the 180-degree preview rotation.
`audio_player_tests` verifies real mpv playback through the current PulseAudio
server using a generated four-second tone (5% volume); it needs mpv installed.
Hardware camera/display tests remain separate from cloud tests.
The scanner test executable redirects camera process launches to its own YUV/QR
fixture, exercising real pipes and child cleanup without requiring cloud hardware.
Both QR engines are checked against the same URL, Unicode, long-text,
nearest-center and scan-region fixtures. The pipe/child fixture verifies QR
success retains capture, a rescan and decoder/rate/crop changes reuse it, and
changed capture settings or exit replace/reap the child.

## Troubleshooting

- Close other camera applications before starting a scan.
- On a camera error, check the connection and permissions, then press OK.
- If recognition is unreliable, improve lighting, avoid glare, and adjust the
  distance. The IMX219 may require manual focus adjustment for close objects.
- The guide box is a positioning aid, not a crop or an autofocus indicator.

## License

MIT; see [LICENSE](LICENSE). Template notices and third-party asset licenses are
retained. Dynamically linked ZBar is LGPL-2.1 and ZXing-C++ is Apache-2.0;
their Debian runtime packages provide the corresponding license notices.
