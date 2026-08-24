# Start the Kimodo sidecar by hand, for when the editor is not driving it.
# The editor launches the same script itself; this is the manual/debug entry point.
#
# This repository is the plugin root; Scripts/run_sidecar.sh is canonical.
$ErrorActionPreference = "Stop"

function ConvertTo-WslPath([string] $WindowsPath) {
    $normalized = $WindowsPath.Replace('\', '/')
    if ($normalized -notmatch '^([A-Za-z]):(.*)$') {
        throw "Expected an absolute Windows path, got: $WindowsPath"
    }

    $drive = $Matches[1].ToLowerInvariant()
    return "/mnt/$drive$($Matches[2])"
}

$pluginRootWindows = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$pluginRootWsl = ConvertTo-WslPath $pluginRootWindows
$scriptWsl = ConvertTo-WslPath (Join-Path $pluginRootWindows "Scripts\run_sidecar.sh")

$env:MOCARA_ROOT = $pluginRootWsl
$env:WSLENV = "HF_TOKEN/u:MOCARA_PORT/u:MOCARA_ROOT/u"
wsl -d Ubuntu -- bash -o pipefail -lc "sed 's/\r$//' '$scriptWsl' | bash"
