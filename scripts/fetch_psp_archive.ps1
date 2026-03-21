$ErrorActionPreference = "Stop"
$org = "PSP-Archive"
$dest = "C:\Users\beelink\Desktop\moonlight3\PSP-Archive-Resources"
if (-not (Test-Path $dest)) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
}
Set-Location $dest

$keywords = @("h264", "avc", "mpeg", "media", "video", "decoder", "audio", "atrac", "mp3", "net", "wifi", "wlan", "wpa", "kernel", "prx", "sdk", "lib", "hook", "exploit", "firmware")
$exclude = @("game", "theme", "emul", "lua", "python", "ruby", "bor", "quake", "doom", "hexen")

$page = 1
$repos = @()

Write-Host "Fetching repositories from $org..."
while ($true) {
    $url = "https://api.github.com/orgs/$org/repos?per_page=100&page=$page"
    try {
        $response = Invoke-RestMethod -Uri $url -ErrorAction Stop
        if (-not $response -or $response.Count -eq 0) { break }
        $repos += $response
        $page++
    } catch {
        Write-Host "Error fetching page $page`: $_"
        break
    }
}

Write-Host "Found $($repos.Count) total repositories."

$usefulRepos = @()
foreach ($repo in $repos) {
    if (-not $repo.name) { continue }
    $matched = $false
    $name = $repo.name.ToLower()
    $desc = if ($repo.description) { $repo.description.ToLower() } else { "" }

    foreach ($kw in $keywords) {
        if ($name -match "(^|[^a-z])$kw([^a-z]|$)" -or $desc -match "(^|[^a-z])$kw([^a-z]|$)") {
            $matched = $true
            break
        }
    }

    foreach ($ex in $exclude) {
        if ($name -match $ex -or $desc -match $ex) {
            $matched = $false
            break
        }
    }

    if ($name -in @("ark-4", "ark-dev-sdk", "pspsdk", "wpa2forpsp", "psplinkusb", "psp-media-engine")) {
        $matched = $true
    }

    if ($matched) {
        $usefulRepos += $repo
    }
}

Write-Host "Identified $($usefulRepos.Count) potentially useful repositories."

foreach ($repo in $usefulRepos) {
    if (-not (Test-Path $repo.name)) {
        Write-Host "Cloning $($repo.name)..."
        try {
            git clone $repo.clone_url -q
        } catch {
            Write-Host "Failed to clone $($repo.name)"
        }
    }
}
Write-Host "All useful repositories processed."
