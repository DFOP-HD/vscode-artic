# Windows counterpart of `package.sh install`: builds the server, packages the extension
# and installs the VSIX into the running editor. Skips `npm install` when node_modules is
# already there, so a repeat run is just a rebuild + package.
$ErrorActionPreference = 'Stop'

Write-Host '==> Building Artic LSP server'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-lsp.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Server build failed' }

Push-Location $PSScriptRoot
try {
    if (-not (Test-Path (Join-Path $PSScriptRoot 'node_modules'))) {
        Write-Host '==> Installing extension dependencies'
        npm install
        if ($LASTEXITCODE -ne 0) { throw 'npm install failed' }
    }

    Write-Host '==> Packaging extension (.vsix)'
    npm run package
    if ($LASTEXITCODE -ne 0) { throw 'vsce package failed' }

    $vsix = Get-ChildItem -Path $PSScriptRoot -Filter 'artic-language-server-*.vsix' -File |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $vsix) { throw 'No .vsix was produced' }
}
finally {
    Pop-Location
}

# In a remote window the first `code` on PATH is the server CLI, which installs on the
# host holding the workspace -- that is the one we want.
$cli = Get-Command code, code.cmd, cursor -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $cli) {
    Write-Host "Package created: $($vsix.FullName)"
    throw 'No editor CLI (code/cursor) on PATH -- install the VSIX manually'
}

Write-Host "==> Installing $($vsix.Name) via $($cli.Source)"
& $cli.Source --install-extension $vsix.FullName --force
if ($LASTEXITCODE -ne 0) { throw 'Extension install failed' }

Write-Host 'Done. Reload the window to pick up the new server.'
