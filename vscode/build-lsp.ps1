# Windows counterpart of build-lsp.sh: builds the language server and stages it
# where the extension looks for it (vscode/build/bin/artic-lsp.exe).
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'artic-lsp'

# Reuse an already-configured build tree so pressing F5 doesn't trigger a fresh
# dependency fetch. Falls back to configuring artic-lsp/build with the default generator.
$buildDir = $null
foreach ($candidate in 'buildGcc', 'buildVS2022', 'build') {
    $p = Join-Path $source $candidate
    if (Test-Path (Join-Path $p 'CMakeCache.txt')) { $buildDir = $p; break }
}
if (-not $buildDir) {
    $buildDir = Join-Path $source 'build'
    cmake -S $source -B $buildDir -D CMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}

cmake --build $buildDir --target artic-lsp --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$exe = Get-ChildItem -Path $buildDir -Recurse -Filter 'artic-lsp.exe' -File |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) { throw "artic-lsp.exe was not produced under $buildDir" }

$dest = Join-Path $PSScriptRoot 'build/bin'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Force $exe.FullName (Join-Path $dest 'artic-lsp.exe')
Write-Host "Staged $($exe.FullName) -> $dest"
