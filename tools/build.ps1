param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\$Configuration"
$luau = Join-Path $root "extern\luau"

if (-not (Test-Path (Join-Path $luau "Ast\src\Parser.cpp"))) {
    throw "Luau sources are missing; run 'git submodule update --init --recursive'."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the C++ x64 tools was found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

foreach ($dir in "luau", "core", "dll", "tests") { New-Item -ItemType Directory -Force (Join-Path $out $dir) | Out-Null }

$flags = if ($Configuration -eq "Release") { "/O2 /MT /DNDEBUG" } else { "/Od /MTd /Zi /D_DEBUG" }
$common = "/nologo /c /std:c++20 /EHsc /permissive- /utf-8 /Zc:__cplusplus /DNOMINMAX $flags"
$include = "/I`"$root\include`" /I`"$root\src`" /external:W0 /external:I`"$luau\Ast\include`" /external:I`"$luau\Common\include`""

$luauSources = @(
    "Common\src\BytecodeWire.cpp", "Common\src\StringUtils.cpp", "Common\src\TimeTrace.cpp",
    "Ast\src\Allocator.cpp", "Ast\src\Ast.cpp", "Ast\src\Confusables.cpp", "Ast\src\Cst.cpp",
    "Ast\src\Lexer.cpp", "Ast\src\Location.cpp", "Ast\src\Parser.cpp", "Ast\src\PrettyPrinter.cpp"
) | ForEach-Object { "`"$luau\$_`"" }
$coreSources = @("Catalog.cpp", "Compiler.cpp", "Definitions.cpp", "Emitter.cpp", "Program.cpp", "TypeSystem.cpp") | ForEach-Object { "`"$root\src\$_`"" }

$script = @"
@echo off
call "$vcvars" >nul || exit /b 1
cl $common /w $include /Fo"$out\luau\\" $($luauSources -join ' ') || exit /b 1
cl $common /W4 /WX $include /Fo"$out\core\\" $($coreSources -join ' ') || exit /b 1
cl $common /W4 /WX $include /DUDONLUAU_SHARED /DUDONLUAU_EXPORTS /Fo"$out\dll\\" "$root\src\CApi.cpp" || exit /b 1
lib /nologo /OUT:"$out\Luau.Ast.lib" "$out\luau\*.obj" || exit /b 1
lib /nologo /OUT:"$out\UdonLuau.Core.lib" "$out\core\*.obj" || exit /b 1
link /nologo /DLL /OUT:"$out\UdonLuau.dll" "$out\dll\CApi.obj" "$out\UdonLuau.Core.lib" "$out\Luau.Ast.lib" || exit /b 1
cl $common /W4 $include /Fo"$out\tests\\" "$root\tests\Tests.cpp" || exit /b 1
link /nologo /OUT:"$out\UdonLuau.Tests.exe" "$out\tests\Tests.obj" "$out\UdonLuau.Core.lib" "$out\Luau.Ast.lib" || exit /b 1
"@

$batch = Join-Path $out "build.cmd"
Set-Content -Path $batch -Value $script -Encoding ascii
& cmd /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if (-not $SkipTests) {
    & (Join-Path $out "UdonLuau.Tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
}

Write-Host "Built $Configuration into $out"
