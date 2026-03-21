# Recursive comparison script for libgamestream directories
# n3ds-moonlight: moonlight-N3DS-n3ds-main/libgamestream/
# moonlight-psp:   psp_moonlight/src/libgamestream/

$workspace = Get-Location
$n3dsLibgamestream = Join-Path $workspace "moonlight-N3DS-n3ds-main\libgamestream"
$pspLibgamestream = Join-Path $workspace "psp_moonlight\src\libgamestream"

# Check if directories exist
if (-not (Test-Path $n3dsLibgamestream)) {
    Write-Error "Directory not found: $n3dsLibgamestream"
    exit 1
}
if (-not (Test-Path $pspLibgamestream)) {
    Write-Error "Directory not found: $pspLibgamestream"
    exit 1
}

# Get all files recursively in each directory with relative paths
Write-Host "Scanning n3ds-moonlight libgamestream..." -ForegroundColor Cyan
$n3dsFiles = Get-ChildItem -Path $n3dsLibgamestream -File -Recurse | Select-Object -ExpandProperty FullName | ForEach-Object { 
    $_.Substring($n3dsLibgamestream.Length + 1) 
}
Write-Host "Scanning moonlight-psp libgamestream..." -ForegroundColor Cyan
$pspFiles = Get-ChildItem -Path $pspLibgamestream -File -Recurse | Select-Object -ExpandProperty FullName | ForEach-Object { 
    $_.Substring($pspLibgamestream.Length + 1) 
}

# Find files only in each directory
$onlyInN3DS = $n3dsFiles | Where-Object { -not ($pspFiles -contains $_) }
$onlyInPSP = $pspFiles | Where-Object { -not ($n3dsFiles -contains $_) }
$commonFiles = $n3dsFiles | Where-Object { $pspFiles -contains $_ }

Write-Host "Found $($n3dsFiles.Count) files in n3ds-moonlight libgamestream"
Write-Host "Found $($pspFiles.Count) files in moonlight-psp libgamestream"
Write-Host "Only in n3ds-moonlight: $($onlyInN3DS.Count)"
Write-Host "Only in moonlight-psp: $($onlyInPSP.Count)"
Write-Host "Common files: $($commonFiles.Count)"

# Initialize result collections
$identical = @()
$different = @()

# Compare common files
foreach ($relativePath in $commonFiles) {
    $n3dsFile = Join-Path $n3dsLibgamestream $relativePath
    $pspFile = Join-Path $pspLibgamestream $relativePath
    
    try {
        # Read both files as byte arrays
        $bytes1 = [System.IO.File]::ReadAllBytes($n3dsFile)
        $bytes2 = [System.IO.File]::ReadAllBytes($pspFile)
        
        if ($bytes1 -eq $bytes2) {
            # Byte-for-byte identical
            $identical += $relativePath
        } else {
            # Check if both files are valid UTF8 without loss
            $valid1 = $false
            $valid2 = $false
            $text1 = $null
            $text2 = $null
            
            try {
                $text1 = [System.Text.Encoding]::UTF8.GetString($bytes1)
                $roundtrip1 = [System.Text.Encoding]::UTF8.GetBytes($text1)
                $valid1 = ($roundtrip1 -eq $bytes1)
            } catch {
                $valid1 = $false
            }
            
            try {
                $text2 = [System.Text.Encoding]::UTF8.GetString($bytes2)
                $roundtrip2 = [System.Text.Encoding]::UTF8.GetBytes($text2)
                $valid2 = ($roundtrip2 -eq $bytes2)
            } catch {
                $valid2 = $false
            }
            
            if ($valid1 -and $valid2) {
                # Both are valid UTF8, compare lines (normalizing line endings)
                $lines1 = $text1 -split "`r?`n"
                $lines2 = $text2 -split "`r?`n"
                if ($lines1 -eq $lines2) {
                    $identical += $relativePath
                } else {
                    $different += @{ Path = $relativePath; Reason = "Text content differs" }
                }
            } else {
                # At least one file is not valid UTF8 (or lossy conversion) -> treat as binary difference
                $different += @{ Path = $relativePath; Reason = "Binary content differs" }
            }
        }
    } catch {
        # Error reading files
        $different += @{ Path = $relativePath; Reason = "Error reading files: $_" }
    }
}

# Build result object
$result = @{
    onlyInN3DS = $onlyInN3DS | Sort-Object
    onlyInPSP = $onlyInPSP | Sort-Object
    identical = $identical | Sort-Object
    different = $different | Sort-Object Path
}

# Convert to JSON and output
$json = $result | ConvertTo-Json -Depth 5
Write-Output $json