param(
    [string]$SourceFile = ''
)
# Builds the headset binaries bundled into the APK:
#   build/android/qpro-camd                           MJPEG camera daemon (static libjpeg-turbo)
#   build/android/questpro-camera-injector            injects the streamer into the camera service
#   build/android/libquestpro-camera-streamer-v9.so   streamer library (shared-file copy, legacy --source shared)
#   build/android/libquestpro-camera-streamer-v12.so  camera buffer handoff (what the app injects)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Android SDK: local.properties sdk.dir, then ANDROID_HOME / ANDROID_SDK_ROOT, then Android Studio's default.
$sdk = $null
$localProperties = Join-Path (Split-Path -Parent $root) 'local.properties'
if (Test-Path -LiteralPath $localProperties) {
    $line = Select-String -LiteralPath $localProperties -Pattern '^sdk\.dir=(.+)$' | Select-Object -First 1
    if ($line) { $sdk = $line.Matches[0].Groups[1].Value -replace '\\:', ':' -replace '\\\\', '\' }
}
foreach ($candidate in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android\Sdk'))) {
    if (-not $sdk -and $candidate -and (Test-Path -LiteralPath $candidate)) { $sdk = $candidate }
}

# NDK: ANDROID_NDK_HOME, else the newest NDK installed in the SDK, else the old fixed path.
$ndk = $env:ANDROID_NDK_HOME
if ([string]::IsNullOrWhiteSpace($ndk) -and $sdk -and (Test-Path -LiteralPath (Join-Path $sdk 'ndk'))) {
    $newest = Get-ChildItem -LiteralPath (Join-Path $sdk 'ndk') -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'build\cmake\android.toolchain.cmake') } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($newest) { $ndk = $newest.FullName }
}
if ([string]::IsNullOrWhiteSpace($ndk)) { $ndk = 'E:\SDKs\Android\android-ndk-r30' }

# CMake: PATH, else the SDK's CMake, else the usual install folders.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($cmake) -and $sdk -and (Test-Path -LiteralPath (Join-Path $sdk 'cmake'))) {
    $sdkCmake = Get-ChildItem -LiteralPath (Join-Path $sdk 'cmake') -Directory | Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\cmake.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($sdkCmake) { $cmake = $sdkCmake }
}
foreach ($candidate in @('C:\Program Files\CMake\bin\cmake.exe', 'E:\Program Files\CMake\bin\cmake.exe')) {
    if ([string]::IsNullOrWhiteSpace($cmake) -and (Test-Path -LiteralPath $candidate)) { $cmake = $candidate }
}
if ([string]::IsNullOrWhiteSpace($cmake)) { Write-Error 'CMake not found: install it (cmake.org) or through Android Studio SDK Manager > SDK Tools > CMake'; exit 1 }
$clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$make = Join-Path $ndk 'prebuilt\windows-x86_64\bin\make.exe'
$toolchain = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
$source = Join-Path $root 'third-party\libjpeg-turbo'
$libBuild = Join-Path $root 'build\android\libjpeg-turbo'
$outDir = Join-Path $root 'build\android'
$output = Join-Path $outDir 'qpro-camd'
if ([string]::IsNullOrWhiteSpace($SourceFile)) { $SourceFile = Join-Path $root 'daemon\qpro_camd.c' }
try {
    if (-not (Test-Path -LiteralPath $clang)) { throw "Android NDK not found (looked in $ndk): install it in Android Studio SDK Manager > SDK Tools > NDK, or set ANDROID_NDK_HOME" }
    if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) { throw "libjpeg-turbo submodule missing: run git submodule update --init" }
    New-Item -ItemType Directory -Force -Path $libBuild | Out-Null
    & $cmake -S $source -B $libBuild -G 'Unix Makefiles' "-DCMAKE_MAKE_PROGRAM=$make" '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-28' '-DANDROID_TOOLCHAIN=clang' '-DCMAKE_ASM_FLAGS=--target=aarch64-linux-android28' "-DCMAKE_TOOLCHAIN_FILE=$toolchain" '-DCMAKE_BUILD_TYPE=Release' '-DENABLE_SHARED=OFF' '-DENABLE_STATIC=ON' '-DWITH_TURBOJPEG=ON' '-DWITH_TOOLS=OFF' '-DWITH_TESTS=OFF'
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build $libBuild --target turbojpeg-static --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "libturbojpeg build failed: $LASTEXITCODE" }
    $library = Join-Path $libBuild 'libturbojpeg.a'
    if (-not (Test-Path -LiteralPath $library)) { throw "Static TurboJPEG library missing: $library" }
    & $clang --target=aarch64-linux-android28 -std=c11 -O3 -Wall -Wextra -Werror -fPIE -pie '-Wl,-z,max-page-size=16384' '-Wl,--strip-all' '-I' (Join-Path $source 'src') $SourceFile (Join-Path $root 'daemon\area_resize.c') (Join-Path $root 'daemon\onboard_tongue.c') $library -pthread -lm -ldl -o $output
    if ($LASTEXITCODE -ne 0) { throw "qpro-camd compile/link failed: $LASTEXITCODE" }
    & $clang --target=aarch64-linux-android28 -std=c11 -O3 -Wall -Wextra -fPIC -shared '-Wl,-z,max-page-size=16384' (Join-Path $root 'streamer\streamer.c') -o (Join-Path $outDir 'libquestpro-camera-streamer-v9.so')
    if ($LASTEXITCODE -ne 0) { throw "streamer compile failed: $LASTEXITCODE" }
    # v12 hands the camera buffers to qpro-camd instead of copying frames; the app ships this one.
    & $clang --target=aarch64-linux-android28 -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared '-Wl,-z,max-page-size=16384' (Join-Path $root 'streamer\streamer_handoff.c') -o (Join-Path $outDir 'libquestpro-camera-streamer-v12.so')
    if ($LASTEXITCODE -ne 0) { throw "handoff streamer compile failed: $LASTEXITCODE" }
    & $clang --target=aarch64-linux-android28 -std=c11 -O2 -Wall -Wextra -fPIE -pie '-Wl,-z,max-page-size=16384' (Join-Path $root 'streamer\injector.c') -o (Join-Path $outDir 'questpro-camera-injector') -ldl
    if ($LASTEXITCODE -ne 0) { throw "injector compile failed: $LASTEXITCODE" }
    Write-Host "BUILT $outDir"
    exit 0
} catch {
    Write-Error $_
    exit 1
}
