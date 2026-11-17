# NitLink device info
# Reads a few facts about this PC's capture cards and writes them to one text file on the Desktop.
# It changes nothing, needs no administrator rights and sends nothing anywhere. You send the file yourself.
#
# How to run: right-click the Start button > Terminal (or Windows PowerShell), paste this whole text, press Enter.
$Out = Join-Path ([Environment]::GetFolderPath('Desktop')) 'NitLink-device-info.txt'
if ($env:NITLINK_INFO_OUT) { $Out = $env:NITLINK_INFO_OUT }

$lines = New-Object System.Collections.Generic.List[string]
function Say([string]$text = '') { $lines.Add($text) }
function Section([string]$title) { Say; Say ('=== ' + $title + ' ===') }

Say 'NitLink device info'
Say ('Written: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))

Section 'Windows'
try {
    $os = Get-CimInstance Win32_OperatingSystem
    Say ($os.Caption + ', build ' + $os.BuildNumber)
} catch { Say 'not available' }

Section 'Graphics cards'
try {
    Get-CimInstance Win32_VideoController | ForEach-Object {
        Say ($_.Name + ' | driver ' + $_.DriverVersion + ' | ' + $_.CurrentHorizontalResolution + ' x ' + $_.CurrentVerticalResolution + ' @ ' + $_.CurrentRefreshRate + ' Hz')
    }
} catch { Say 'not available' }

# Windows lists a capture card in one or more groups. NitLink looks in the first group ("camera").
# A card that appears only in the other two is visible to older-style apps but not to NitLink today.
Section 'Capture cards by Windows group'
$groups = [ordered]@{
    'camera (what NitLink sees)' = '{e5323777-f976-4f5b-9b55-b94699c46e44}'
    'capture'                    = '{65e8773d-8f56-11d0-a3b9-00a0c9223196}'
    'video'                      = '{6994ad05-93ef-11d0-a3cc-00a0c9223196}'
}
# Only video devices are listed: the "capture" group also holds sound devices, which are left out.
$videoDevices = @{}
foreach ($name in 'camera (what NitLink sees)', 'video') {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\' + $groups[$name]
    if (Test-Path $key) {
        Get-ChildItem $key -ErrorAction SilentlyContinue | ForEach-Object {
            $instance = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DeviceInstance
            if ($instance) { $videoDevices[$instance.ToUpper()] = $true }
        }
    }
}
foreach ($name in $groups.Keys) {
    Say ('-- ' + $name)
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\' + $groups[$name]
    $found = $false
    if (Test-Path $key) {
        Get-ChildItem $key -ErrorAction SilentlyContinue | ForEach-Object {
            $instance = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DeviceInstance
            if ($instance -and $videoDevices.ContainsKey($instance.ToUpper())) {
                $friendly = ''
                try { $friendly = (Get-PnpDevice -InstanceId $instance -ErrorAction Stop).FriendlyName } catch { }
                $present = $false
                try { $present = (Get-PnpDevice -InstanceId $instance -ErrorAction Stop).Present } catch { }
                Say ('   ' + $(if ($present) { 'connected ' } else { 'not connected ' }) + '| ' + $friendly + ' | ' + $instance)
                $found = $true
            }
        }
    }
    if (-not $found) { Say '   none' }
}

Section 'Those video devices: status and driver'
try {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $videoDevices.ContainsKey($_.InstanceId.ToUpper()) } | Sort-Object Class, FriendlyName | ForEach-Object {
        $provider = ''; $version = ''
        try {
            $provider = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_DriverProvider' -ErrorAction Stop).Data
            $version = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction Stop).Data
        } catch { }
        Say ($_.Class + ' | ' + $_.FriendlyName + ' | ' + $_.Status + ' | driver ' + $provider + ' ' + $version)
    }
} catch { Say 'not available' }

Section 'Video inputs older-style apps can open (DirectShow)'
$shown = $false
foreach ($root in 'HKLM:\SOFTWARE\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance', 'HKCU:\SOFTWARE\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance') {
    if (Test-Path $root) {
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $friendly = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).FriendlyName
            if ($friendly) { Say ('   ' + $friendly); $shown = $true }
        }
    }
}
if (-not $shown) { Say '   none registered here (cards usually appear in the groups above instead)' }

Section 'NitLink'
$process = Get-Process -Name 'NitLink-FrameGen-Local', 'NitLink' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($process -and $process.Path) {
    Say 'NitLink is running.'
    $log = Join-Path (Split-Path $process.Path) 'nitlink-session.log'
    if (Test-Path $log) {
        Say '-- lines about capture devices from the session log'
        $profilePath = [Environment]::GetFolderPath('UserProfile')
        Get-Content $log -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'device|Device|Negotiated|capture card|No capture|Opening|formats at|failed|Failed' } |
            Select-Object -First 120 | ForEach-Object { Say ('   ' + $_.Replace($profilePath, '%USERPROFILE%')) }
    } else { Say 'No session log found beside the program.' }
} else {
    Say 'NitLink is not running. Start it, then run this again, so its log is included.'
}

$lines | Set-Content -Path $Out -Encoding UTF8
Write-Host ''
Write-Host ('Done. The file is here: ' + $Out)
Write-Host 'Open it, check you are happy with what it contains, and send it.'
