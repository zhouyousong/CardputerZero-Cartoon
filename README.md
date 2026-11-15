# CardputerZero-Cartoon

A camera QR audio player for the 320x170 CardputerZero, based on the
[CardputerZero LVGL template](https://github.com/CardputerZero/Template).

## Interaction

- Launch **CardputerZero-Cartoon** into an idle, fixed two-column screen.
  The left 160x120 camera tile is gray whenever scanning is inactive.
  The right side always shows up to three recent audio tracks, newest first,
  the selected track highlighted, playback status, elapsed time and progress.
- **8 / OK / Enter**: stop audio and start a new scan.
- A grayscale preview runs at up to 10 fps; QR decoding runs at up to 5 fps.
  The full 320x240 grayscale camera frame is decoded, not just the guide box.
- The preview is rotated 180 degrees to correct the mounted camera orientation.
  QR decoding still receives the unmodified camera frame.
- On a valid audio QR, decoding pauses and audio starts automatically.
  The closest QR code to the scan region center wins when several are visible.
- **6**: pause/resume scanning or audio; retry the current operation on errors.
  Pausing scanning discards queued results and keeps the camera warm for 30 seconds.
- **5 / 7** (also **Left / Right**): previous/next track in the visible history, immediately
  stopping the current operation and playing the chosen track from the start.
  History does not wrap at the ends.
- **R**: replay the selected track from the start.
- **Up / Down**: volume, in steps of 5%, bounded to 0-100%.
- **4** (also **S**): scanner settings (see below).
- **ESC**: exit and release both camera and player.

The five bottom hints align with the physical 4-8 keys rather than equal-width
screen columns. Replay, volume and exit hints stay in the header.
After recognition or scan pause, the camera tile is gray and QR decoding/preview
conversion stop, but capture continues in memory for up to 30 seconds. Frames
are drained and discarded to avoid blocking the camera pipe. A scan within that
window reuses capture when settings are unchanged, discards the in-flight old
frame, and displays the first fresh preview before decoding it. Switching/replaying
history does not extend the standby deadline. Timeout or exit releases capture;
the first scan, a scan after timeout, or changed settings require a cold start.
Warm standby still occupies the camera and consumes power and memory.

Audio sources must be an absolute Linux file path (for example
`/home/pi/audio/story.mp3`) or an HTTP/HTTPS direct audio URL. Unicode file names
and URL query strings are preserved. Relative paths, other schemes, multiline
QR text and URLs containing embedded credentials are rejected explicitly.
Ordinary HTML pages are not resolved to audio; there is no browser or shell
execution of QR content. mpv handles supported audio formats with video disabled.

The three-item history lives only in memory. Duplicate scans move the existing
source to the top; playback alone never reorders it. A syntactically valid but
unplayable source stays selected for retry. Playback ends without looping or
restarting the camera. Unknown duration is shown as `--:--`, not a fake progress.

## Scanner settings

Press **4** for the separate settings page. Entering it pauses scanning/audio;
returning does not automatically resume either. **F/X** selects a row,
**Z/C** changes it, **OK** saves and returns, and **ESC** cancels changes.
Arrow keys remain equivalent alternatives.
New values apply on the next scan:

- Capture resolution: 320x240 (default), 480x360, 640x480, 800x600, or 1280x960.
  Preview remains 160x120; higher capture sizes cost more memory and decoding time.
- QR decode rate: up to 2, 5 (default), or 10 scans per second; camera preview
  remains up to 10 fps, subject to device performance.
- Exposure compensation: -2 to +2 EV, default 0 (not optical focus).
- Decode region: full frame (default) or the central 50% width and height.
  The guide outlines the actual region; preview orientation remains 180 degrees.

Settings persist atomically in `$XDG_CONFIG_HOME/cardputerzero-cartoon/scanner.conf`
or `$HOME/.config/cardputerzero-cartoon/scanner.conf`. Missing settings use defaults;
unreadable or corrupt settings report an error and can be replaced through **4**.
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
simultaneously; warm camera capture can coexist with playback for 30 seconds.
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
    libzbar-dev libqrencode-dev libcjson-dev dpkg-dev
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
deduplicated history, bounded previous/next, replay, retry, volume and exit.
Settings tests cover persistence and malformed input; region tests verify
center-only cropping and fixed preview scaling for the capture resolutions. An asymmetric four-corner
frame checks the 180-degree preview rotation.
`audio_player_tests` verifies real mpv playback through the current PulseAudio
server using a generated four-second tone (5% volume); it needs mpv installed.
Hardware camera/display tests remain separate from cloud tests.

## Troubleshooting

- Close other camera applications before starting a scan.
- On a camera error, check the connection and permissions, then press OK.
- If recognition is unreliable, improve lighting, avoid glare, and adjust the
  distance. The IMX219 may require manual focus adjustment for close objects.
- The guide box is a positioning aid, not a crop or an autofocus indicator.

## License

MIT; see [LICENSE](LICENSE). Template notices and third-party asset licenses are
retained.
