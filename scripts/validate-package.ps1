param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $Path).ProviderPath.TrimEnd('\')
if ((Get-Item -LiteralPath $packageRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Package root cannot be a reparse point.'
}
$required = @(
    'NitLink.exe', 'nitlink-menu.html', 'locales/en-US.js', 'locales/zh-TW.js', 'locales/zh-CN.js', 'locales/de-DE.js', 'locales/ko-KR.js',
    'assets/menu/menu.js', 'assets/menu/Archivo-400.ttf', 'assets/menu/Archivo-500.ttf',
    'assets/menu/Archivo-600.ttf', 'assets/menu/Archivo-OFL.txt', 'assets/menu/kofi-cup.png',
    'third_party/nis/NIS_Scaler.h', 'third_party/nis/LICENSE.txt',
    'LICENSE', 'LICENSES.md', 'ACKNOWLEDGMENTS.md'
)
$runtimeDlls = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll')
$allowed = $required + $runtimeDlls + @('README.txt', 'RELEASE-NOTES.md', 'docs/4ks-hdr-tonemap.md', 'SHA256SUMS.txt')
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($packageRoot)
$files = @{}
while ($pending.Count) {
    foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in package: $($item.Name)" }
        if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
        $relative = $item.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
        if ($relative -notin $allowed) { throw "Unexpected package file: $relative" }
        if ($item.Length -eq 0 -or $item.Length -gt 128MB) { throw "Invalid package file size: $relative" }
        $files[$relative] = $item.FullName
    }
}
foreach ($relative in $required) {
    if (!$files.ContainsKey($relative)) { throw "Missing required package file: $relative" }
}
foreach ($relative in $runtimeDlls) {
    if (!$files.ContainsKey($relative)) { continue }
    $signature = Get-AuthenticodeSignature -LiteralPath $files[$relative]
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:,|$)') {
        throw "Runtime DLL lacks a valid Microsoft signature: $relative"
    }
}
# Checksums detect corruption and record package contents; they do not replace
# publisher signing or authenticate files distributed with this same manifest.
$lines = foreach ($relative in ($files.Keys | Where-Object { $_ -ne 'SHA256SUMS.txt' } | Sort-Object)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $files[$relative] -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
[IO.File]::WriteAllLines((Join-Path $packageRoot 'SHA256SUMS.txt'), [string[]]$lines, [Text.UTF8Encoding]::new($false))
Write-Output "Validated $($lines.Count) package files and wrote SHA256SUMS.txt."
