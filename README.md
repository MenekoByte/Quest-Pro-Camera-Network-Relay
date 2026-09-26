# Quest Pro Camera Service

Streams the Meta Quest Pro's inward-facing IR cameras (eyes, lower face, brow) over Wi-Fi as MJPEG, from a small app on the headset. Anything on your network can read the streams: Baballonia, [Qpro-Enhanced-FT (Wi-Fi fork)](https://github.com/MonadoArt/Qpro-Enhanced-FT-Wireless-MJPEG), or a browser.

## Requirements

- Quest Pro, rooted with Magisk
- USB or wireless ADB to install the APK

The service attaches to the headset's camera service the same way [Qpro-Enhanced-FT](https://github.com/n0tmast3r/Qpro-Enhanced-FT) does, using its streamer and injector.

## Install

1. Download the APK from [Releases](../../releases).
2. Install it: `adb install -r QuestProCameraService.apk`
3. On the headset open Library > Unknown Sources > Quest Pro Camera Service and allow superuser access when Magisk asks.

Updates install over the previous version with the same command.

Release APKs are signed with this certificate (SHA-256):

```
4C:83:4F:99:9C:D3:60:AE:11:3D:93:F1:E3:2C:12:A1:41:6A:4E:53:8B:F4:22:57:BE:96:6C:43:D5:D0:70:93
```

Check a download with `apksigner verify --print-certs QuestProCameraService.apk`.

## Use

Press **Start**. The app can then be closed or left running in the background; the service keeps running until you press **Stop** or reboot.

The cameras only produce frames while face tracking is active, for example Virtual Desktop or Steam Link with face tracking on and focused. Until then the app shows "Waiting for camera frames".

Streams are served on port 27280 of the headset's IP. The app shows the address.

| Path | Content |
| --- | --- |
| `/camera0.mjpg` ... `/camera4.mjpg` | Left eye, right eye, left face, right face, brow (400x400 each) |
| `/eyes.mjpg` | Both eye cameras side by side, same frame (800x400) |
| `/mouth.mjpg` | Both lower-face cameras side by side, same frame (800x400) |
| `/face.mjpg` | Lower-face pair and brow (1200x400) |
| `/strip.mjpg` | All five cameras (2000x400) |
| `/camera0.jpg` ... `/camera4.jpg` | Single current frame |
| `/status` | JSON: state, frame rates, CPU use, encode times |

Each MJPEG part carries `X-Sequence` and `X-Timestamp-Ns` headers. Only streams with a viewer are encoded; with no viewers the service idles.

For Baballonia, add a camera with the URL `http://<headset-ip>:27280/camera2.mjpg` (or `camera3`).

The streams have no authentication. Use them on a network you trust.

## Build

Requirements: Android NDK (r30 tested), CMake, and Android Studio or the Android SDK with JDK 17+.

```
git clone --recurse-submodules https://github.com/MonadoArt/Quest-Pro-Camera-Network-Relay.git
```

Then double-click **Build Release.bat**. It finds Java, builds everything and opens the `release` folder with `QuestProCameraService.apk` and its SHA-256 (full log in `release\build.log`). Android Studio's Build button and `gradlew assembleDebug` / `assembleRelease` work too.

Every APK build first runs `native/build.ps1` (skipped when the native sources are unchanged), which builds the camera daemon (libjpeg-turbo linked statically), the streamers and the injector. It finds the NDK and CMake in the Android SDK; set `ANDROID_NDK_HOME` to use a different NDK.

Release builds are signed with your local Android debug key, so your own builds update each other but not the official releases. To sign with a different key, create `keystore.properties` in the repo root (it is git-ignored):

```
storeFile=path/to/release.jks
storePassword=...
keyAlias=...
keyPassword=...
```

Daemon tests (Linux or WSL with gcc, make, cmake, python3 and curl): `bash native/tests/run_host_test.sh`

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
