# Diagnostic Chicken agent
# Read-only. Listens on this PC only (127.0.0.1:47321) and stops when you close the window.
# It does not change services, drivers, or startup.
#
# Detects the CPU and GPU, and refreshes load and temperature while this window stays open.
# CPU die temperature needs LibreHardwareMonitor with its WMI provider enabled.
# NVIDIA temperature uses nvidia-smi. AMD and Intel temperatures need LibreHardwareMonitor.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\sentinel-agent.ps1

$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$Port = 47321

function Convert-Num($value) {
  if ($null -eq $value) { return $null }
  $text = ([string]$value).Trim()
  if ($text -eq "" -or $text -eq "N/A" -or $text -eq "[N/A]") { return $null }
  try { return [double]$text } catch { return $null }
}

function Add-Note([System.Collections.Generic.List[string]]$list, [string]$note) {
  if ($note -and -not $list.Contains($note)) { [void]$list.Add($note) }
}

function Test-SensorType($sensor, [string]$name, [string]$code) {
  $kind = [string]$sensor.SensorType
  return ($kind -eq $name -or $kind -eq $code)
}

function Expand-Sensors {
  $dest = Join-Path $PSScriptRoot "sensors"
  $dll = Join-Path $dest "LibreHardwareMonitorLib.dll"
  if (Test-Path $dll) { return }
  $zip = Join-Path $PSScriptRoot "sensors.zip"
  if (-not (Test-Path $zip)) { return }
  if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
}

function Enable-SensorFlag($computer, [string]$name) {
  $prop = $computer.GetType().GetProperty($name)
  if ($prop) { $prop.SetValue($computer, $true, $null) }
}

function Add-HardwareSensors($found, $hardware) {
  try { $hardware.Update() } catch {}
  foreach ($sensor in @($hardware.Sensors)) {
    $found.Add([pscustomobject]@{
        Name       = [string]$sensor.Name
        SensorType = [string]$sensor.SensorType
        Value      = $sensor.Value
        Parent     = [string]$hardware.Name
      })
  }
  $subs = @()
  try { $subs = @($hardware.SubHardware) } catch { $subs = @() }
  foreach ($sub in $subs) {
    if ($sub) { Add-HardwareSensors $found $sub }
  }
}

function Get-LibrarySensors {
  if ($script:SensorLibraryFailed) { return @() }
  Expand-Sensors
  $dll = Join-Path $PSScriptRoot "sensors\LibreHardwareMonitorLib.dll"
  if (-not (Test-Path $dll)) { return @() }
  try {
    if (-not $script:SensorComputer) {
      [void][System.Reflection.Assembly]::LoadFrom($dll)
      $computer = New-Object LibreHardwareMonitor.Hardware.Computer
      foreach ($flag in @("IsCpuEnabled", "IsCPUEnabled", "IsGpuEnabled", "IsGPUEnabled", "IsMotherboardEnabled", "IsStorageEnabled", "IsMemoryEnabled")) {
        Enable-SensorFlag $computer $flag
      }
      $computer.Open()
      $script:SensorComputer = $computer
    }
    $found = New-Object System.Collections.Generic.List[object]
    foreach ($hardware in @($script:SensorComputer.Hardware)) {
      if ($hardware) { Add-HardwareSensors $found $hardware }
    }
    return @($found.ToArray())
  } catch {
    $script:SensorLibraryFailed = $true
    $script:SensorLibraryError = $_.Exception.Message
    return @()
  }
}

function Get-SensorPack {
  if ($script:UseLibrary) { return @(Get-LibrarySensors) }
  $cache = Join-Path $PSScriptRoot "sensor-cache.json"
  if (Test-Path $cache) {
    try {
      $parsed = Get-Content -LiteralPath $cache -Raw -ErrorAction Stop | ConvertFrom-Json
      if ($null -eq $parsed) { return @() }
      return @($parsed)
    } catch {}
  }
  return @()
}

function Get-NvidiaSmiPath {
  $cmd = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $candidate = Join-Path $env:SystemRoot "System32\nvidia-smi.exe"
  if (Test-Path $candidate) { return $candidate }
  return $null
}

function Get-CpuIdentity {
  if ($script:CpuIdentity) { return $script:CpuIdentity }
  try {
    $processors = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
    if ($processors.Count -eq 0) { return $null }
    $first = $processors[0]
    $script:CpuIdentity = [ordered]@{
      name    = ([string]$first.Name).Trim()
      cores   = [int]$first.NumberOfCores
      threads = [int]$first.NumberOfLogicalProcessors
      maxMhz  = [int]$first.MaxClockSpeed
    }
    return $script:CpuIdentity
  } catch {
    return $null
  }
}

function Get-CpuLoad {
  try {
    $processors = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
    $loads = @($processors | Where-Object { $null -ne $_.LoadPercentage } | ForEach-Object { [double]$_.LoadPercentage })
    if ($loads.Count -eq 0) { return $null }
    return [int][math]::Round((($loads | Measure-Object -Average).Average), 0)
  } catch {
    return $null
  }
}

function Get-GpuIdentity {
  if ($script:GpuIdentity) { return $script:GpuIdentity }
  $found = @()
  try {
    $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction Stop)
    foreach ($adapter in $adapters) {
      $name = ([string]$adapter.Name).Trim()
      if (-not $name) { continue }
      if ($name -match "Microsoft Basic|Remote Display|Indirect Display|Virtual Display|Virtual Monitor|Meta Virtual|Parsec|IddCx|Oray|Sunshine|GameViewer|Mirror|Oculus|Quest|Spacedesk|AnyDesk|TeamViewer|DameWare|Citrix|Hyper-V|Basic Render") { continue }
      $found += [ordered]@{
        name   = $name
        driver = ([string]$adapter.DriverVersion).Trim()
      }
    }
  } catch {
    $found = @()
  }
  if ($found.Count -eq 0) {
    $found += [ordered]@{ name = $null; driver = $null }
  }
  $script:GpuIdentity = $found
  return $script:GpuIdentity
}

function Get-GpuSensorRank([string]$name) {
  if ($name -match "Memory|Junction|Hot Spot|Hotspot") { return 1 }
  if ($name -match "Core|Edge|Die|GPU Temperature|^GPU$") { return 4 }
  if ($name -match "SoC|SOC|VR") { return 2 }
  if ($name -match "GPU|Temp") { return 3 }
  return 0
}

function Get-MatchingGpuSensor($sensors, [string]$cardName) {
  $best = $null
  $bestRank = -1
  $bestTemp = -1.0
  foreach ($sensor in @($sensors)) {
    if (-not (Test-SensorType $sensor "Temperature" "2")) { continue }
    $name = [string]$sensor.Name
    $parent = [string]$sensor.Parent
    if ("$parent $name" -notmatch "GPU|Radeon|GeForce|Arc|NVIDIA|NVidia") { continue }
    $rank = Get-GpuSensorRank $name
    if ($rank -le 0) { continue }
    $temp = Convert-Num $sensor.Value
    if ($null -eq $temp -or $temp -lt 1 -or $temp -gt 125) { continue }
    if ($cardName) {
      $tokens = @($cardName.Split(" ") | Where-Object { $_.Length -ge 3 -and $_ -notmatch "^(NVIDIA|GeForce|Radeon|Graphics|Intel|AMD|Adapter|GPU)$" })
      foreach ($token in $tokens) {
        $token = $token -replace "[^A-Za-z0-9]", ""
        if ($token.Length -ge 3 -and ("$parent $name" -like "*$token*")) { $rank += 3; break }
      }
    }
    if ($rank -gt $bestRank -or ($rank -eq $bestRank -and $temp -gt $bestTemp)) {
      $bestRank = $rank
      $bestTemp = [double]$temp
      $best = $sensor
    }
  }
  return $best
}

function Add-MonitorGpu($gpus, [string]$name, [string]$driver, $tempC, [string]$source, $load) {
  if ($name -match "Microsoft Basic|Remote Display|Indirect Display|Virtual Display|Virtual Monitor|Meta Virtual|Parsec|IddCx|Oray|Sunshine|GameViewer|Mirror|Oculus|Quest|Spacedesk|AnyDesk|TeamViewer|DameWare|Citrix|Hyper-V|Basic Render") { return }
  foreach ($have in @($gpus)) {
    if ($have.name -and $name -and ($have.name -eq $name -or $have.name -like "*$name*" -or $name -like "*$($have.name)*")) { return }
  }
  $gpus.Add([ordered]@{
      name        = $name
      driver      = $driver
      loadPercent = $load
      tempC       = $tempC
      tempSource  = $source
    })
}

function Add-LibraryGpus($gpus, $sensors) {
  $best = @{}
  foreach ($sensor in @($sensors)) {
    if (-not (Test-SensorType $sensor "Temperature" "2")) { continue }
    $sensorName = [string]$sensor.Name
    $parent = [string]$sensor.Parent
    if (-not $parent) { continue }
    if ("$parent $sensorName" -notmatch "GPU|Radeon|GeForce|Arc|UHD|Iris|Graphics|NVIDIA") { continue }
    if ((Get-GpuSensorRank $sensorName) -le 0) { continue }
    $temp = Convert-Num $sensor.Value
    if ($null -eq $temp -or $temp -lt 1 -or $temp -gt 125) { continue }
    $key = $parent.ToLower()
    if (-not $best.ContainsKey($key) -or $temp -gt $best[$key]) { $best[$key] = [math]::Round([double]$temp, 1) }
  }
  foreach ($key in @($best.Keys)) {
    $parent = $key
    foreach ($sensor in @($sensors)) {
      if (([string]$sensor.Parent).ToLower() -eq $key) { $parent = [string]$sensor.Parent; break }
    }
    Add-MonitorGpu $gpus $parent $null $best[$key] "librehardwaremonitor" $null
  }
}

function Get-LiveReading {
  $notes = New-Object System.Collections.Generic.List[string]
  $identity = Get-CpuIdentity
  $cpuName = $null
  $cores = $null
  $threads = $null
  $maxMhz = $null
  if ($identity) {
    $cpuName = $identity.name
    $cores = $identity.cores
    $threads = $identity.threads
    $maxMhz = $identity.maxMhz
  } else {
    Add-Note $notes "Could not read the CPU from WMI."
  }

  $cpuTemp = $null
  $cpuTempSource = "unavailable"
  $lhm = Get-SensorPack
  $cpuSensor = $lhm | Where-Object { (Test-SensorType $_ "Temperature" "2") -and ([string]$_.Name -match "Package") } | Select-Object -First 1
  if (-not $cpuSensor) {
    $cpuSensor = $lhm | Where-Object {
      (Test-SensorType $_ "Temperature" "2") -and (([string]$_.Name -match "CPU") -or ([string]$_.Parent -match "CPU|Ryzen|Core"))
    } | Select-Object -First 1
  }
  if ($cpuSensor) {
    $cpuTemp = [math]::Round((Convert-Num $cpuSensor.Value), 1)
    $cpuTempSource = "librehardwaremonitor"
  }

  if ($null -eq $cpuTemp) {
    try {
      $zones = @(Get-CimInstance -Namespace "root/wmi" -ClassName MSAcpi_ThermalZoneTemperature -ErrorAction Stop)
      $best = $null
      $bestName = $null
      foreach ($zone in $zones) {
        $celsius = Convert-Num $zone.CurrentTemperature
        if ($null -eq $celsius) { continue }
        $celsius = [math]::Round(($celsius / 10.0) - 273.15, 1)
        if ($celsius -lt 10 -or $celsius -gt 120) { continue }
        if ($null -eq $best -or $celsius -gt $best) {
          $best = $celsius
          $bestName = [string]$zone.InstanceName
        }
      }
      if ($null -ne $best) {
        $cpuTemp = $best
        $cpuTempSource = "acpi-thermal-zone"
        Add-Note $notes "CPU figure is ACPI thermal zone $bestName, not the CPU die."
      }
    } catch {}
  }
  if ($null -eq $cpuTemp) {
    $errFile = Join-Path $PSScriptRoot "sensor-error.txt"
    if (Test-Path $errFile) {
      $detail = ((Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue) | Out-String).Trim()
      if ($detail) { Add-Note $notes "The built-in sensor reader failed. $detail" }
      else { Add-Note $notes "The built-in sensor reader failed. Load and the log check still work." }
    } else {
      Add-Note $notes "CPU die temperature is still coming up. The log check does not wait on it."
    }
  }

  $nvidia = @()
  $smi = Get-NvidiaSmiPath
  if ($smi) {
    try {
      $info = New-Object System.Diagnostics.ProcessStartInfo
      $info.FileName = $smi
      $info.Arguments = "--query-gpu=name,temperature.gpu,utilization.gpu,driver_version --format=csv,noheader,nounits"
      $info.RedirectStandardOutput = $true
      $info.UseShellExecute = $false
      $info.CreateNoWindow = $true
      $proc = New-Object System.Diagnostics.Process
      $proc.StartInfo = $info
      [void]$proc.Start()
      if (-not $proc.WaitForExit(1500)) {
        try { $proc.Kill() } catch {}
      } else {
        foreach ($line in @($proc.StandardOutput.ReadToEnd() -split "`r?`n")) {
          if (-not $line -or $line -match "^\s*$") { continue }
          $parts = @($line.Split(","))
          if ($parts.Count -lt 2) { continue }
          $util = $null
          $driver = $null
          if ($parts.Count -ge 3) { $util = Convert-Num $parts[2] }
          if ($parts.Count -ge 4) { $driver = $parts[3].Trim() }
          $nvidia += [ordered]@{
            name   = $parts[0].Trim()
            temp   = Convert-Num $parts[1]
            util   = $util
            driver = $driver
          }
        }
      }
    } catch {
      Add-Note $notes "nvidia-smi was found but did not return a reading."
    }
  }

  $gpus = New-Object System.Collections.Generic.List[object]
  Add-LibraryGpus $gpus $lhm
  foreach ($card in @($nvidia)) {
    $tempC = $null
    $tempSource = "unavailable"
    if ($null -ne $card.temp -and [double]$card.temp -ge 1) {
      $tempC = [math]::Round([double]$card.temp, 1)
      $tempSource = "nvidia-smi"
    }
    $load = $null
    if ($null -ne $card.util) { $load = [int][math]::Round([double]$card.util, 0) }
    if ($null -eq $tempC) {
      $sensor = Get-MatchingGpuSensor $lhm ([string]$card.name)
      if ($sensor) {
        $tempC = [math]::Round((Convert-Num $sensor.Value), 1)
        $tempSource = "librehardwaremonitor"
      }
    }
    Add-MonitorGpu $gpus ([string]$card.name) ([string]$card.driver) $tempC $tempSource $load
  }
  foreach ($adapter in @(Get-GpuIdentity)) {
    if (-not $adapter.name) { continue }
    $sensor = Get-MatchingGpuSensor $lhm ([string]$adapter.name)
    $tempC = $null
    $source = "unavailable"
    if ($sensor) {
      $tempC = [math]::Round((Convert-Num $sensor.Value), 1)
      $source = "librehardwaremonitor"
    }
    Add-MonitorGpu $gpus ([string]$adapter.name) ([string]$adapter.driver) $tempC $source $null
  }
  if ($gpus.Count -eq 0) {
    Add-Note $notes "No display adapter was detected."
    $gpus.Add([ordered]@{
        name        = $null
        driver      = $null
        loadPercent = $null
        tempC       = $null
        tempSource  = "unavailable"
      })
  } elseif (-not @($gpus | Where-Object { $null -ne $_.tempC })) {
    Add-Note $notes "No GPU temperature yet. The log check does not wait on it."
  }

  $statusFile = Join-Path $PSScriptRoot "sensor-status.txt"
  if (Test-Path $statusFile) {
    $status = ((Get-Content -LiteralPath $statusFile -Raw -ErrorAction SilentlyContinue) | Out-String).Trim()
    if ($status) { Add-Note $notes $status }
  }

  $cpu = [ordered]@{
    name        = $cpuName
    cores       = $cores
    threads     = $threads
    maxMhz      = $maxMhz
    loadPercent = Get-CpuLoad
    tempC       = $cpuTemp
    tempSource  = $cpuTempSource
  }

  return [ordered]@{
    kind        = "sentinel-agent"
    version     = 1
    host        = $env:COMPUTERNAME
    collectedAt = (Get-Date).ToUniversalTime().ToString("o")
    cpu         = $cpu
    gpus        = @($gpus.ToArray())
    notes       = @($notes.ToArray())
  }
}

function Get-Diagnostic {
  $notes = New-Object System.Collections.Generic.List[string]
  $events = New-Object System.Collections.Generic.List[object]
  $start = (Get-Date).AddDays(-2)
  foreach ($log in @("System", "Application")) {
    $rows = @()
    try {
      $rows = @(Get-WinEvent -FilterHashtable @{ LogName = $log; Level = 1, 2, 3; StartTime = $start } -MaxEvents 40 -ErrorAction Stop)
    } catch {
      $message = $_.Exception.Message
      if ($message -notmatch "No events were found") {
        Add-Note $notes "Could not read the $log log. $message"
      }
    }
    foreach ($row in $rows) {
      $levelName = "Information"
      switch ([int]$row.Level) {
        1 { $levelName = "Critical" }
        2 { $levelName = "Error" }
        3 { $levelName = "Warning" }
      }
      $text = [string]$row.Message
      if ($text.Length -gt 700) { $text = $text.Substring(0, 700) }
      $events.Add([ordered]@{
          log     = $log
          time    = $row.TimeCreated.ToUniversalTime().ToString("o")
          level   = $levelName
          source  = [string]$row.ProviderName
          eventId = [int]$row.Id
          message = $text
        })
    }
  }
  if ($events.Count -eq 0) {
    Add-Note $notes "No critical, error, or warning events in System or Application over the last 2 days."
  }
  return [ordered]@{
    kind        = "sentinel-report"
    version     = 1
    host        = $env:COMPUTERNAME
    collectedAt = (Get-Date).ToUniversalTime().ToString("o")
    notes       = @($notes.ToArray())
    events      = @($events.ToArray())
  }
}

function Send-Http($stream, [int]$code, [string]$body, [string]$contentType = "application/json; charset=utf-8") {
  $payload = [System.Text.Encoding]::UTF8.GetBytes($body)
  $reason = "OK"
  if ($code -eq 204) { $reason = "No Content" }
  if ($code -eq 404) { $reason = "Not Found" }
  if ($code -eq 500) { $reason = "Error" }
  $header = "HTTP/1.1 $code $reason`r`nConnection: close`r`nCache-Control: no-store`r`nAccess-Control-Allow-Origin: *`r`nAccess-Control-Allow-Private-Network: true`r`nAccess-Control-Allow-Methods: GET, OPTIONS`r`nAccess-Control-Allow-Headers: Content-Type`r`nContent-Type: $contentType`r`nContent-Length: $($payload.Length)`r`n`r`n"
  $headBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
  $stream.Write($headBytes, 0, $headBytes.Length)
  if ($payload.Length -gt 0) { $stream.Write($payload, 0, $payload.Length) }
  $stream.Flush()
}

function Read-Asset([string]$name) {
  $path = Join-Path $PSScriptRoot $name
  if (-not (Test-Path $path)) { return $null }
  return [System.IO.File]::ReadAllText($path)
}

function Read-RequestPath($stream) {
  $buffer = New-Object byte[] 2048
  $read = $stream.Read($buffer, 0, $buffer.Length)
  if ($read -le 0) { return $null }
  $text = [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
  if ($text -notmatch '^(GET|OPTIONS)\s+(\S+)') { return $null }
  return @{ method = $Matches[1]; path = (($Matches[2] -split "\?")[0]) }
}

if ($args -contains "-SensorWorker") {
  $ErrorActionPreference = "Continue"
  $script:UseLibrary = $false
  function Write-Live {
    try {
      $json = Get-LiveReading | ConvertTo-Json -Depth 6 -Compress
      $path = Join-Path $PSScriptRoot "live.json"
      $tmp = "$path.tmp"
      [System.IO.File]::WriteAllText($tmp, $json)
      if (Test-Path $path) { Remove-Item -LiteralPath $path -Force }
      Move-Item -LiteralPath $tmp -Destination $path -Force
    } catch {
      try { [System.IO.File]::WriteAllText((Join-Path $PSScriptRoot "sensor-error.txt"), $_.Exception.Message) } catch {}
    }
  }
  function Install-PawnIo {
    if ($script:PawnTried) { return $false }
    $script:PawnTried = $true
    if (Get-Service -Name "PawnIO" -ErrorAction SilentlyContinue) { return $true }
    $status = Join-Path $PSScriptRoot "sensor-status.txt"
    $problem = Join-Path $PSScriptRoot "sensor-error.txt"
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
      [System.IO.File]::WriteAllText($status, "Approve the administrator prompt so the CPU and GPU sensors can be read.")
      return $false
    }
    [System.IO.File]::WriteAllText($status, "Installing the sensor driver. Temperatures update after it finishes.")
    Write-Live
    $setup = Join-Path $PSScriptRoot "sensors\PawnIO_setup.exe"
    try {
      Expand-Sensors
      [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
      Invoke-WebRequest -Uri "https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe" -OutFile $setup -UseBasicParsing
      Start-Process -FilePath $setup -ArgumentList "-install", "-silent" -Wait -WindowStyle Hidden | Out-Null
      Start-Sleep -Seconds 2
    } catch {
      [System.IO.File]::WriteAllText($problem, $_.Exception.Message)
    }
    if (Test-Path $status) { Remove-Item -LiteralPath $status -Force }
    return [bool](Get-Service -Name "PawnIO" -ErrorAction SilentlyContinue)
  }
  Write-Live
  Install-PawnIo | Out-Null
  $script:UseLibrary = $true
  while ($true) {
    Write-Live
    Start-Sleep -Seconds 2
  }
}

$listener = $null
try {
  $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
  $listener.Start()
} catch {
  Write-Host "Could not listen on 127.0.0.1:$Port. $($_.Exception.Message)"
  Write-Host "Close the other Diagnostic Chicken window if one is already open."
  exit 1
}

$script:SensorProc = $null
$script:SensorNext = Get-Date

function Start-SensorWorker {
  try {
    if ($script:SensorProc -and -not $script:SensorProc.HasExited) { return }
    if ((Get-Date) -lt $script:SensorNext) { return }
    $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    $script:SensorProc = Start-Process -FilePath $ps -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru -ArgumentList @(
      "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath, "-SensorWorker"
    )
    $script:SensorNext = (Get-Date).AddSeconds(8)
  } catch {}
}

Start-SensorWorker

function Read-LiveJson {
  $path = Join-Path $PSScriptRoot "live.json"
  if (Test-Path $path) {
    try { return [System.IO.File]::ReadAllText($path) } catch {}
  }
  $hostName = $env:COMPUTERNAME
  return "{`"kind`":`"sentinel-agent`",`"host`":`"$hostName`",`"cpu`":{`"name`":null,`"tempC`":null,`"tempSource`":`"unavailable`",`"loadPercent`":null},`"gpus`":[{`"name`":null,`"tempC`":null,`"tempSource`":`"unavailable`",`"driver`":null,`"loadPercent`":null}],`"notes`":[`"Sensors are still starting.`"]}"
}

try {
  while ($true) {
    $client = $listener.AcceptTcpClient()
    Start-SensorWorker
    $client.ReceiveTimeout = 5000
    $client.SendTimeout = 20000
    $stream = $null
    try {
      $stream = $client.GetStream()
      $stream.ReadTimeout = 5000
      $request = Read-RequestPath $stream
      if ($null -eq $request) {
        Send-Http $stream 404 '{"ok":false}'
      } elseif ($request.method -eq "OPTIONS") {
        Send-Http $stream 204 ""
      } elseif ($request.path -eq "/health") {
        Send-Http $stream 200 '{"ok":true,"kind":"sentinel-agent"}'
      } elseif ($request.path -eq "/" -or $request.path -eq "/desk") {
        $html = Read-Asset "desk.html"
        if (-not $html) { Send-Http $stream 500 '{"ok":false,"error":"desk missing"}' }
        else { Send-Http $stream 200 $html "text/html; charset=utf-8" }
      } elseif ($request.path -eq "/playbooks.json") {
        $book = Read-Asset "playbooks.json"
        if (-not $book) { Send-Http $stream 200 "[]" }
        else { Send-Http $stream 200 $book "application/json; charset=utf-8" }
      } elseif ($request.path -eq "/live") {
        Send-Http $stream 200 (Read-LiveJson)
      } elseif ($request.path -eq "/diagnose") {
        Write-Host "$(Get-Date -Format HH:mm:ss)  diagnostic"
        $json = (Get-Diagnostic | ConvertTo-Json -Depth 6 -Compress)
        Send-Http $stream 200 $json
      } else {
        Send-Http $stream 404 '{"ok":false}'
      }
    } catch {
      if ($stream) {
        try { Send-Http $stream 500 '{"ok":false,"error":"request failed"}' } catch {}
      }
    } finally {
      if ($stream) { $stream.Dispose() }
      $client.Close()
    }
  }
} finally {
  if ($listener) { $listener.Stop() }
}
