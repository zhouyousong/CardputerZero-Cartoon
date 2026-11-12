# CardputerZero-Cartoon

An offline camera QR scanner for the 320x170 CardputerZero, based on the
[CardputerZero LVGL template](https://github.com/CardputerZero/Template).

## Interaction

- Launch **CardputerZero-Cartoon** from APPLaunch to start the camera.
- A grayscale preview runs at up to 10 fps; QR decoding runs at up to 5 fps.
  The full 320x240 grayscale camera frame is decoded, not just the guide box.
- The preview is rotated 180 degrees to correct the mounted camera orientation.
  QR decoding still receives the unmodified camera frame.
- On recognition, scanning stops, the camera is released, and the complete text
  is displayed. The closest QR code to the image center wins if multiple codes
  are visible.
- **OK / Enter**: scan again from a result, or retry a camera error.
- **5**: pause/resume a live scan. It does not dismiss a result.
- **Up / Down**: scroll long results or camera errors.
- **ESC**: exit and release the camera.

UTF-8 text, including Chinese and line breaks, is displayed without truncation.
Non-displayable binary/control data is reported as unsupported. Web links are
identified but never opened automatically.

## Privacy

Frames and decoded results stay in device memory. The app does not save images,
record video, access QR URLs, or upload results. Diagnostic logs contain camera
errors and result byte counts, never decoded text.

## Hardware and runtime

The initial target is the IMX219 camera on CardputerZero with Debian 13 ARM64.
Capture uses the device's existing `/usr/bin/rpicam-vid` and libcamera.
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
3. Run QR decoding and interaction tests.
4. Package a `.deb` and upload `cardputerzero-cartoon-arm64`.

No local compiler or SDK is needed.

## Native Linux build

On Debian 13 ARM64:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config git \
    libfmt-dev libpng-dev libjpeg-dev libfreetype-dev zlib1g-dev \
    libzbar-dev libqrencode-dev dpkg-dev
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
cover pause, queued-result suppression, OK-only rescan, scrolling, error recovery,
and exit. An asymmetric four-corner frame checks the 180-degree preview rotation.
Hardware camera/display tests remain separate from cloud tests.

## Troubleshooting

- Close other camera applications before starting a scan.
- On a camera error, check the connection and permissions, then press OK.
- If recognition is unreliable, improve lighting, avoid glare, and adjust the
  distance. The IMX219 may require manual focus adjustment for close objects.
- The guide box is a positioning aid, not a crop or an autofocus indicator.

## Original Camera compatibility repair

The separate `repair/camera-libcamera08` branch builds the original
[CardputerZero Camera v1.0.3](https://github.com/CardputerZero/Camera/tree/v1.0.3)
against Raspberry Pi libcamera `0.8.0+rpt20261104-1`. The source commit is pinned
in `.github/workflows/build-camera-compat.yml`; the patch updates the runtime
dependency and renamed libcamera 0.8 metering/white-balance enums without
changing their meaning. The result is a custom
`camera` package version `1.0.3-m5stack1+libcamera08.1`, not an official release.

Use this package only on a matching ARM64 system. Simulate installation with
`apt-get -s --no-remove --no-install-recommends install /absolute/package.deb`
before installing. It should update only `camera`, without downgrading the
camera stack or changing Cartoon. Photos are not part of this package.

## License

MIT; see [LICENSE](LICENSE). Template notices and third-party asset licenses are
retained.
