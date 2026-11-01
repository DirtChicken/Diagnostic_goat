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
  $builtIn = @(Get-LibrarySensors)
  if ($builtIn.Count -gt 0) { return $builtIn }
  try {
    return @(Get-CimInstance -Namespace "root\LibreHardwareMonitor" -ClassName Sensor -ErrorAction Stop)
  } catch {
    return @()
  }
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
      if ($name -match "Microsoft Basic|Remote Display|Indirect Display|Virtual Display|Parsec|IddCx|Oray|Sunshine|GameViewer|Mirror") { continue }
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
    if ($script:SensorLibraryError) {
      Add-Note $notes "The built-in sensor reader did not start. $($script:SensorLibraryError)"
    } else {
      Add-Note $notes "No CPU die temperature yet. Approve the administrator prompt so Diagnostic Chicken can read it."
    }
  }

  $nvidia = @()
  $smi = Get-NvidiaSmiPath
  if ($smi) {
    try {
      $raw = & $smi --query-gpu=name,temperature.gpu,utilization.gpu,driver_version --format=csv,noheader,nounits 2>$null
      foreach ($line in @($raw)) {
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
    } catch {
      Add-Note $notes "nvidia-smi was found but did not return a reading."
    }
  }

  $adapters = @(Get-GpuIdentity)
  $gpus = New-Object System.Collections.Generic.List[object]
  if ($nvidia.Count -gt 0) {
    foreach ($card in $nvidia) {
      $tempSource = "unavailable"
      $tempC = $null
      if ($null -ne $card.temp) {
        $tempC = [math]::Round([double]$card.temp, 1)
        $tempSource = "nvidia-smi"
      }
      $load = $null
      if ($null -ne $card.util) { $load = [int][math]::Round([double]$card.util, 0) }
      $gpus.Add([ordered]@{
          name        = [string]$card.name
          driver      = [string]$card.driver
          loadPercent = $load
          tempC       = $tempC
          tempSource  = $tempSource
        })
    }
  } else {
    $gpuTemp = $null
    $gpuTempSource = "unavailable"
    $gpuLoad = $null
    $gpuSensor = $lhm | Where-Object {
      (Test-SensorType $_ "Temperature" "2") -and (([string]$_.Name -match "GPU Core") -or (([string]$_.Parent -match "GPU|Radeon|GeForce|Arc") -and ([string]$_.Name -match "Core|GPU")))
    } | Select-Object -First 1
    $loadSensor = $lhm | Where-Object {
      (Test-SensorType $_ "Load" "3") -and ([string]$_.Name -match "GPU Core|GPU")
    } | Select-Object -First 1
    if ($gpuSensor) {
      $gpuTemp = [math]::Round((Convert-Num $gpuSensor.Value), 1)
      $gpuTempSource = "librehardwaremonitor"
    }
    if ($loadSensor) {
      $gpuLoad = [int][math]::Round((Convert-Num $loadSensor.Value), 0)
    }
    $named = @($adapters | Where-Object { $_.name })
    if ($named.Count -eq 0 -and $gpuSensor) {
      $gpus.Add([ordered]@{
          name        = [string]$gpuSensor.Parent
          driver      = $null
          loadPercent = $gpuLoad
          tempC       = $gpuTemp
          tempSource  = $gpuTempSource
        })
    } else {
      $index = 0
      foreach ($adapter in $named) {
        $tempC = $null
        $source = "unavailable"
        $load = $null
        if ($index -eq 0) {
          $tempC = $gpuTemp
          $source = $gpuTempSource
          $load = $gpuLoad
        }
        $gpus.Add([ordered]@{
            name        = [string]$adapter.name
            driver      = [string]$adapter.driver
            loadPercent = $load
            tempC       = $tempC
            tempSource  = $source
          })
        $index += 1
      }
    }
    if ($null -eq $gpuTemp) {
      Add-Note $notes "No GPU temperature from the built-in reader. An NVIDIA card can still report through nvidia-smi."
    }
  }

  if ($gpus.Count -gt 1) {
    Add-Note $notes "$($gpus.Count) GPUs detected. The desk shows the hottest one."
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

$listener = $null
try {
  $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
  $listener.Start()
} catch {
  Write-Host "Could not listen on 127.0.0.1:$Port. $($_.Exception.Message)"
  Write-Host "Close the other Diagnostic Chicken window if one is already open."
  exit 1
}

Write-Host ""
Write-Host "Diagnostic Chicken"
Write-Host "Listening on http://127.0.0.1:$Port/  (this PC only)"
Write-Host "Leave this window open, then open Diagnostic Chicken in a browser on this PC."
Write-Host "Connect, then Run diagnostic for System and Application errors."
Write-Host "Ctrl+C stops the agent. It does not keep running after that."
Write-Host ""

try {
  while ($true) {
    $client = $listener.AcceptTcpClient()
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
        $json = (Get-LiveReading | ConvertTo-Json -Depth 6 -Compress)
        Send-Http $stream 200 $json
      } elseif ($request.path -eq "/diagnose") {
        Write-Host "$(Get-Date -Format HH:mm:ss)  diagnostic"
        $json = (Get-Diagnostic | ConvertTo-Json -Depth 6 -Compress)
        Send-Http $stream 200 $json
      } else {
        Send-Http $stream 404 '{"ok":false}'
      }
    } catch {
      Write-Host "Request failed: $($_.Exception.Message)"
    } finally {
      if ($stream) { $stream.Dispose() }
      $client.Close()
    }
  }
} finally {
  if ($listener) { $listener.Stop() }
}
