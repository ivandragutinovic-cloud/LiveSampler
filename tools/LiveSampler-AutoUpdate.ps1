# Live Sampler: automatski skida najnoviju verziju sa GitHub-a na Desktop (folder "Live Sampler").
# Pokrece ga zakazani zadatak svakih 15 minuta; skida samo kad postoji nova verzija.
$ErrorActionPreference = 'Stop'
$repo    = 'ivandragutinovic-cloud/LiveSampler'
$desktop = [Environment]::GetFolderPath('Desktop')
$target  = Join-Path $desktop 'Live Sampler'
$state   = Join-Path $target '.verzija'

try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/latest" -Headers @{ 'User-Agent' = 'LiveSampler-AutoUpdate' }
} catch { exit 0 }   # nema interneta ili nema verzije

$id = "$($rel.id)"
if ((Test-Path $state) -and ((Get-Content $state -Raw).Trim() -eq $id)) { exit 0 }

New-Item -ItemType Directory -Force -Path $target | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-dd HH-mm'
foreach ($a in $rel.assets) {
    $zip = Join-Path $target $a.name
    Invoke-WebRequest -Uri $a.browser_download_url -OutFile $zip -Headers @{ 'User-Agent' = 'LiveSampler-AutoUpdate' }
    $name = [IO.Path]::GetFileNameWithoutExtension($a.name)
    $dir  = Join-Path $target $name
    # ako program radi, fajl je zakljucan: tada raspakuj u novi folder sa datumom
    try {
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        Expand-Archive -Path $zip -DestinationPath $dir -Force
    } catch {
        Expand-Archive -Path $zip -DestinationPath "$dir $stamp" -Force
    }
}
Set-Content -Path $state -Value $id
Set-Content -Path (Join-Path $target 'NOVA VERZIJA.txt') -Value ("Skinuto: $stamp`r`n" + $rel.body)
