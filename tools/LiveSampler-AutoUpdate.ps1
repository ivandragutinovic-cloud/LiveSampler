# Live Sampler: automatski skida i raspakuje najnoviju verziju direktno u Desktop\Live Sampler.
# Pokrece ga zakazani zadatak svakih 15 minuta; radi samo kad postoji nova verzija.
$ErrorActionPreference = 'Stop'
$repo    = 'ivandragutinovic-cloud/LiveSampler'
$desktop = [Environment]::GetFolderPath('Desktop')
$target  = Join-Path $desktop 'Live Sampler'
$state   = Join-Path $target '.verzija'
$ua      = @{ 'User-Agent' = 'LiveSampler-AutoUpdate' }

# najnovija verzija ove skripte (da ispravke stizu same)
try {
    $self = Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/$repo/main/tools/LiveSampler-AutoUpdate.ps1" -Headers $ua
    if ($self.Content -and $self.Content -match 'LiveSampler-AutoUpdate' -and $self.Content -ne (Get-Content $PSCommandPath -Raw)) {
        Set-Content -Path $PSCommandPath -Value $self.Content -Encoding UTF8
    }
} catch { }

try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/latest" -Headers $ua
} catch { exit 0 }

$id = "$($rel.id)"
if ((Test-Path $state) -and ((Get-Content $state -Raw).Trim() -eq $id)) { exit 0 }

New-Item -ItemType Directory -Force -Path $target | Out-Null
$tmp = Join-Path $env:TEMP 'LiveSampler-update'
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

foreach ($a in $rel.assets) {
    $zip = Join-Path $tmp $a.name
    Invoke-WebRequest -UseBasicParsing -Uri $a.browser_download_url -OutFile $zip -Headers $ua
    Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp 'files') -Force
}

# kopiraj preko starih fajlova; ako je program otvoren (fajl zakljucan), probaj ponovo za 15 minuta
$locked = $false
Get-ChildItem (Join-Path $tmp 'files') | ForEach-Object {
    $dest = Join-Path $target $_.Name
    try {
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        Copy-Item $_.FullName $dest -Recurse -Force
    } catch { $locked = $true }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

# pocisti stare zipove i podfoldere iz prethodne verzije skripte
foreach ($old in 'LiveSampler-Windows.zip','KronosAutosampler-Windows.zip','LiveSampler-Windows','KronosAutosampler-Windows') {
    $p = Join-Path $target $old
    if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
}

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
if ($locked) {
    Set-Content -Path (Join-Path $target 'NOVA VERZIJA.txt') -Value ("Nova verzija ceka ($stamp): zatvori Live Sampler / Kronos Autosampler, instalira se sama za najvise 15 minuta.`r`n`r`n" + $rel.body)
    exit 0
}
Set-Content -Path $state -Value $id
Set-Content -Path (Join-Path $target 'NOVA VERZIJA.txt') -Value ("Instalirano: $stamp`r`n`r`n" + $rel.body)
