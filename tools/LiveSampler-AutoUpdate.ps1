# Pikaso Workstation: automatski skida i raspakuje najnoviju verziju direktno u Desktop\Pikaso Workstation.
# (ime skripte ostaje LiveSampler-AutoUpdate.ps1 jer ga zakazani zadatak tako poziva)
# Pokrece ga zakazani zadatak svakih 15 minuta; radi samo kad postoji nova verzija.
param([switch]$Pokazi)   # -Pokazi: rucna provera (PROVERI NOVU VERZIJU.cmd), ispisuje sta radi
$ErrorActionPreference = 'Stop'
function Say($t) { if ($Pokazi) { Write-Host $t } }
$repo    = 'ivandragutinovic-cloud/LiveSampler'
$desktop = [Environment]::GetFolderPath('Desktop')
$target  = Join-Path $desktop 'Pikaso Workstation'
$oldDir  = Join-Path $desktop 'Live Sampler'
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
} catch { Say 'Ne mogu da proverim GitHub (internet?).'; exit 0 }

$id = "$($rel.id)"
Say ("Najnovija verzija: " + $rel.published_at + "  -  " + $rel.body)
if ((Test-Path $state) -and ((Get-Content $state -Raw).Trim() -eq $id)) { Say 'Vec imas najnoviju verziju.'; exit 0 }
Say 'Skidam novu verziju...'

# stari folder (Live Sampler) postaje Pikaso Workstation
if ((Test-Path $oldDir) -and -not (Test-Path $target)) {
    try { Rename-Item -Path $oldDir -NewName 'Pikaso Workstation' } catch { }
}
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
foreach ($old in 'LiveSampler-Windows.zip','KronosAutosampler-Windows.zip','LiveSampler-Windows','KronosAutosampler-Windows','Live Sampler.exe','Kronos Autosampler.exe') {
    $p = Join-Path $target $old
    if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
}

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
if ($locked) {
    Say 'Neki program je otvoren (Pikaso Workstation ili Pikaso Autosampler): zatvori ga i pokreni proveru ponovo.'
    Set-Content -Path (Join-Path $target 'NOVA VERZIJA.txt') -Value ("Nova verzija ceka ($stamp): zatvori Pikaso Workstation / Pikaso Autosampler, instalira se sama za najvise 15 minuta.`r`n`r`n" + $rel.body)
    exit 0
}
Set-Content -Path $state -Value $id
Say ('Instalirano u ' + $target)
Set-Content -Path (Join-Path $target 'NOVA VERZIJA.txt') -Value ("Instalirano: $stamp`r`n`r`n" + $rel.body)
