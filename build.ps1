param(
    [switch]$Debug
)
# Builds the APK; Gradle builds the native headset binaries first (native/build.ps1).
# Release output: app\build\outputs\apk\release\app-release.apk (signed if keystore.properties exists)
# For a ready-to-publish APK with its checksum, double-click "Build Release.bat" instead.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$task = 'assembleRelease'
if ($Debug) { $task = 'assembleDebug' }
Push-Location $root
try {
    & (Join-Path $root 'gradlew.bat') $task
    if ($LASTEXITCODE -ne 0) { throw "Gradle $task failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
