param(
  [string]$DeviceUrl = "",
  [switch]$Uninstall,
  [switch]$AtStartup
)
$ErrorActionPreference = "Stop"
$TaskName = "SmallTVUsage"

if ($Uninstall) {
  Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
  Write-Host "Removed scheduled task $TaskName."
  exit 0
}

$Repo = Split-Path -Parent $PSScriptRoot
$DaemonDir = Join-Path $Repo "daemon"
$Config = Join-Path $DaemonDir "usage-daemon.json"

if (-not (Test-Path $Config)) {
  Copy-Item (Join-Path $DaemonDir "usage-daemon.example.json") $Config
  Write-Host "Created $Config from the example."
}

if ($DeviceUrl) {
  $cfg = Get-Content -Raw $Config | ConvertFrom-Json
  $cfg.device = $DeviceUrl.Trim()
  $json = $cfg | ConvertTo-Json -Depth 10
  [IO.File]::WriteAllText($Config, $json, (New-Object Text.UTF8Encoding($false)))
  Write-Host "Device URL set to $($cfg.device)."
}

$python = $null
try {
  $python = (& py -3 -c "import sys; print(sys.executable)" 2>$null | Select-Object -Last 1)
  if ($python) { $python = $python.Trim() }
} catch {}
if (-not $python) {
  $python = (Get-Command python.exe -ErrorAction Stop).Source
}
$pythonw = Join-Path (Split-Path $python) "pythonw.exe"
if (-not (Test-Path $pythonw)) {
  throw "pythonw.exe was not found next to $python. Install Python from python.org (not the Store alias)."
}

$action = New-ScheduledTaskAction `
  -Execute $pythonw `
  -Argument "`"$DaemonDir\usage_daemon.py`" --config `"$Config`"" `
  -WorkingDirectory $Repo
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

if ($AtStartup) {
  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if (-not $isAdmin) {
    throw "Run this script as Administrator for -AtStartup."
  }
  $cred = Get-Credential -Message "Windows account used to run the SmallTV usage daemon at startup"
  if (-not $cred) { throw "A Windows account is required for -AtStartup." }
  $trigger = New-ScheduledTaskTrigger -AtStartup
  Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Description "SmallTV usage daemon" -Force `
    -User $cred.UserName -Password $cred.GetNetworkCredential().Password | Out-Null
  Write-Host "Installed $TaskName to run at startup as $($cred.UserName)."
} else {
  $trigger = New-ScheduledTaskTrigger -AtLogOn
  Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Description "SmallTV usage daemon" -Force | Out-Null
  Write-Host "Installed $TaskName to run at login."
}
Start-ScheduledTask -TaskName $TaskName
Write-Host "Run 'daemon\setup_windows.bat' to enter provider credentials."
