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
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($cmake)) { $cmake = 'E:\Program Files\CMake\bin\cmake.exe' }
$ndk = $env:ANDROID_NDK_HOME
if ([string]::IsNullOrWhiteSpace($ndk)) { $ndk = 'E:\SDKs\Android\android-ndk-r30' }
$clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
$make = Join-Path $ndk 'prebuilt\windows-x86_64\bin\make.exe'
$toolchain = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
$source = Join-Path $root 'third-party\libjpeg-turbo'
$libBuild = Join-Path $root 'build\android\libjpeg-turbo'
$outDir = Join-Path $root 'build\android'
$output = Join-Path $outDir 'qpro-camd'
if ([string]::IsNullOrWhiteSpace($SourceFile)) { $SourceFile = Join-Path $root 'daemon\qpro_camd.c' }
try {
    if (-not (Test-Path -LiteralPath $clang)) { throw "NDK clang not found: $clang (set ANDROID_NDK_HOME)" }
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
