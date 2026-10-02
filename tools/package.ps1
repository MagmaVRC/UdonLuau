param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$packageName = "dev.magmavrc.udonluau"
$package = Join-Path $root "unity\$packageName"
$build = Join-Path $root "build\Release"
$dist = Join-Path $root "dist"

if (-not $SkipBuild) { & (Join-Path $PSScriptRoot "build.ps1") -Configuration Release }
$dll = Join-Path $build "UdonLuau.dll"
if (-not (Test-Path $dll)) { throw "build\Release\UdonLuau.dll is missing; run tools\build.ps1 first." }

$version = (Get-Content (Join-Path $package "package.json") -Raw | ConvertFrom-Json).version
Copy-Item $dll (Join-Path $package "Editor\Plugins\x86_64\UdonLuau.dll") -Force

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null

$vpmZip = Join-Path $dist "$packageName-$version.zip"
Compress-Archive -Path (Join-Path $package "*") -DestinationPath $vpmZip -CompressionLevel Optimal

$staging = Join-Path ([System.IO.Path]::GetTempPath()) "udonluau-unitypackage-$([guid]::NewGuid())"
New-Item -ItemType Directory $staging | Out-Null
try {
    $entries = Get-ChildItem $package -Recurse -Force |
        Where-Object { $_.Extension -ne ".meta" -and $_.FullName.Substring($package.Length) -notmatch '~' }
    $guids = foreach ($entry in $entries) {
        $meta = "$($entry.FullName).meta"
        if (-not (Test-Path $meta)) { continue }
        $guid = (Select-String -Path $meta -Pattern '^guid:\s*([0-9a-f]{32})').Matches[0].Groups[1].Value
        $target = Join-Path $staging $guid
        New-Item -ItemType Directory $target | Out-Null
        Copy-Item $meta (Join-Path $target "asset.meta")
        if (-not $entry.PSIsContainer) { Copy-Item $entry.FullName (Join-Path $target "asset") }
        $relative = $entry.FullName.Substring($package.Length + 1).Replace('\', '/')
        [System.IO.File]::WriteAllText((Join-Path $target "pathname"), "Packages/$packageName/$relative")
        $guid
    }
    $list = Join-Path $staging "files.txt"
    [System.IO.File]::WriteAllLines($list, [string[]]$guids)
    $unityPackage = Join-Path $dist "UdonLuau-$version.unitypackage"
    & (Join-Path $env:SystemRoot "System32\tar.exe") -czf $unityPackage -C $staging -T $list
    if ($LASTEXITCODE -ne 0) { throw "Creating the .unitypackage failed." }
}
finally {
    Remove-Item $staging -Recurse -Force
}

$sdk = Join-Path ([System.IO.Path]::GetTempPath()) "udonluau-sdk-$([guid]::NewGuid())"
New-Item -ItemType Directory (Join-Path $sdk "bin"), (Join-Path $sdk "lib"), (Join-Path $sdk "include") | Out-Null
try {
    Copy-Item $dll, (Join-Path $build "UdonLuau.lib") (Join-Path $sdk "bin")
    Copy-Item (Join-Path $build "UdonLuau.Core.lib"), (Join-Path $build "Luau.Ast.lib") (Join-Path $sdk "lib")
    Copy-Item (Join-Path $root "include\UdonLuau") (Join-Path $sdk "include") -Recurse
    Copy-Item (Join-Path $root "LICENSE"), (Join-Path $root "THIRD-PARTY-NOTICES.md"), (Join-Path $root "README.md") $sdk
    Compress-Archive -Path (Join-Path $sdk "*") -DestinationPath (Join-Path $dist "UdonLuau-native-$version-win-x64.zip") -CompressionLevel Optimal
}
finally {
    Remove-Item $sdk -Recurse -Force
}

Get-ChildItem $dist -File | ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" } |
    Set-Content (Join-Path $dist "SHA256SUMS.txt") -Encoding ascii

Get-ChildItem $dist | Format-Table Name, Length -AutoSize
