# SPDX-License-Identifier: GPL-3.0-or-later
<#
Load a PS4 eboot into a Ghidra project, from fake SELF to analysed program, in one go.

    pwsh tools/ghidra/analyze.ps1 -Self work/update/eboot.bin -Name eboot-1.09

Steps (each is a separate tool, see docs/ghidra.md):
  1. tools/unfself.py     fake SELF  -> plain ELF                   work/elf/<Name>.elf
  2. tools/orbis_prep.py  ELF        -> Ghidra-ready ELF + symbols  work/ghidra/<Name>.ghidra.elf / .symbols.tsv
  3. analyzeHeadless      import, run ApplyOrbisSymbols.java before analysis, auto-analyse, save
                          into the Ghidra project work/ghidra/project/Bloodborne
  4. analyzeHeadless      run OrbisReport.java over the saved program  work/ghidra/report-<Name>/

Configuration comes from parameters or, failing those, environment variables:
  -Ghidra   / GHIDRA_INSTALL_DIR   Ghidra install folder (the one holding support\analyzeHeadless.bat)
  -Jdk      / JAVA_HOME            a JDK that Ghidra accepts (Ghidra 12.2 needs JDK 25)
  -Python   / MENSIS_PYTHON        Python with the project's dependencies; defaults to .venv,
                                   then to python on PATH
  -MaxMem   / GHIDRA_HEADLESS_MAXMEM   heap for the headless analyser (default 20G)
#>
param(
    [Parameter(Mandatory)] [string] $Self,
    [Parameter(Mandatory)] [string] $Name,
    [string] $Ghidra = $env:GHIDRA_INSTALL_DIR,
    [string] $Jdk = $env:JAVA_HOME,
    [string] $Python = $env:MENSIS_PYTHON,
    [string] $MaxMem = $(if ($env:GHIDRA_HEADLESS_MAXMEM) { $env:GHIDRA_HEADLESS_MAXMEM } else { "20G" }),
    [int] $MaxCpu = [Math]::Max(1, [Environment]::ProcessorCount - 4),
    [switch] $ReportOnly
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")

if (-not $Ghidra -or -not (Test-Path (Join-Path $Ghidra "support\analyzeHeadless.bat"))) {
    throw "Ghidra not found: pass -Ghidra or set GHIDRA_INSTALL_DIR to the Ghidra install folder."
}
if (-not $Jdk -or -not (Test-Path (Join-Path $Jdk "bin\java.exe"))) {
    throw "JDK not found: pass -Jdk or set JAVA_HOME (Ghidra 12.2 needs JDK 25)."
}
$py = $Python
if (-not $py) {
    $venv = Join-Path $root ".venv\Scripts\python.exe"
    $py = if (Test-Path $venv) { $venv } else { "python" }
}
$work = Join-Path $root "work"
$elf = Join-Path $work "elf\$Name.elf"
$gelf = Join-Path $work "ghidra\$Name.ghidra.elf"
$tsv = Join-Path $work "ghidra\$Name.symbols.tsv"
$nids = Join-Path $work "ref\aerolib.inl"
$proj = Join-Path $work "ghidra\project"
$scripts = $PSScriptRoot
New-Item -ItemType Directory -Force (Split-Path $elf), (Split-Path $gelf), $proj | Out-Null

$env:JAVA_HOME = $Jdk
$env:GHIDRA_HEADLESS_MAXMEM = $MaxMem
$headless = Join-Path $Ghidra "support\analyzeHeadless.bat"

if (-not $ReportOnly) {
    if (-not (Test-Path $nids)) {
        New-Item -ItemType Directory -Force (Split-Path $nids) | Out-Null
        Invoke-WebRequest "https://raw.githubusercontent.com/shadps4-emu/shadPS4/v.0.7.0/src/core/aerolib/aerolib.inl" -OutFile $nids
    }
    & $py -I (Join-Path $root "tools\unfself.py") $Self $elf
    & $py -I (Join-Path $root "tools\orbis_prep.py") $elf $nids $gelf $tsv
    & $headless $proj Bloodborne -import $gelf -overwrite -scriptPath $scripts `
        -preScript ApplyOrbisSymbols.java $tsv -max-cpu $MaxCpu *> (Join-Path $work "ghidra\analyze-$Name.log")
}
& $headless $proj Bloodborne -process "$Name.ghidra.elf" -noanalysis -readOnly -scriptPath $scripts `
    -postScript OrbisReport.java (Join-Path $work "ghidra\report-$Name") *> (Join-Path $work "ghidra\report-$Name.log")
