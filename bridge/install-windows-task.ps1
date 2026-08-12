# Makes the bridge start at logon.
#
#   powershell -ExecutionPolicy Bypass -File .\install-windows-task.ps1
#   powershell -ExecutionPolicy Bypass -File .\install-windows-task.ps1 -Remove
#
# Prefers a scheduled task, which can restart the bridge if it dies. That call
# needs elevation on many machines, so when it is refused this falls back to a
# shortcut in the Startup folder: no admin rights, same "runs at logon" result,
# minus the automatic restart.
#
param(
    [switch]$Remove,
    [string]$TaskName = 'ClaudeUsageBridge'
)

$ErrorActionPreference = 'Stop'

$startupLink = Join-Path ([Environment]::GetFolderPath('Startup')) 'Claude Usage Bridge.lnk'

function Install-StartupShortcut {
    param([string]$Target)

    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut($startupLink)
    $s.TargetPath       = $Target
    $s.WorkingDirectory = Split-Path $Target
    $s.WindowStyle      = 7   # minimised, so it does not take the foreground
    $s.Description      = 'Serves Claude plan usage to the ESP32-S3 dashboard'
    $s.Save()

    Write-Host "Installed a Startup shortcut: $startupLink"
    Write-Host 'The bridge will start at every logon. Remove it with -Remove.'
}

if ($Remove) {
    $removed = $false
    try {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction Stop
        Write-Host "Removed scheduled task '$TaskName'."
        $removed = $true
    } catch {}
    if (Test-Path $startupLink) {
        Remove-Item $startupLink -Force
        Write-Host "Removed Startup shortcut."
        $removed = $true
    }
    if (-not $removed) { Write-Host 'Nothing installed to remove.' }
    return
}

$node = (Get-Command node -ErrorAction SilentlyContinue).Source
if (-not $node) { throw 'node was not found on PATH. Install Node 18 or newer first.' }

$script = Join-Path $PSScriptRoot 'bridge.mjs'
if (-not (Test-Path $script)) { throw "bridge.mjs not found next to this script ($PSScriptRoot)." }

$action    = New-ScheduledTaskAction -Execute $node -Argument "`"$script`"" -WorkingDirectory $PSScriptRoot
$trigger   = New-ScheduledTaskTrigger -AtLogOn
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

try {
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
        -Settings $settings -Principal $principal -Force -ErrorAction Stop | Out-Null

    Write-Host "Registered '$TaskName'. Starting it now..."
    Start-ScheduledTask -TaskName $TaskName
    Write-Host 'Done. The bridge will start automatically at every logon.'
}
catch {
    # Registering a task is refused without elevation on many machines. The
    # Startup folder needs no rights at all and reaches the same result, so
    # fall back rather than sending the user off to find an admin shell.
    Write-Host "Could not register a scheduled task ($($_.Exception.Message.Trim()))."
    Write-Host 'Falling back to the Startup folder, which needs no admin rights.'

    $launcher = Join-Path $PSScriptRoot 'start-bridge.cmd'
    if (-not (Test-Path $launcher)) { throw "start-bridge.cmd not found in $PSScriptRoot." }
    Install-StartupShortcut -Target $launcher
}
