# Registers the bridge as a Windows scheduled task that starts at logon.
#
#   powershell -ExecutionPolicy Bypass -File .\install-windows-task.ps1
#   powershell -ExecutionPolicy Bypass -File .\install-windows-task.ps1 -Remove
#
param(
    [switch]$Remove,
    [string]$TaskName = 'ClaudeUsageBridge'
)

$ErrorActionPreference = 'Stop'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed scheduled task '$TaskName'."
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

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

Write-Host "Registered '$TaskName'. Starting it now..."
Start-ScheduledTask -TaskName $TaskName
Write-Host 'Done. The bridge will start automatically at every logon.'
