param(
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

Write-Host "=== Super Mario Kart (SNESRecomp Native AOT) Builder ===" -ForegroundColor Cyan

if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

Push-Location "build"
try {
    cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=$BuildType
    ninja smk_play
    Write-Host "Build successful! Executable: build/smk_play.exe" -ForegroundColor Green
}
finally {
    Pop-Location
}
