# Recursive comparison script for PSP-Archive-Resources and psp_moonlight directories

$workspace = Get-Location
$pspArchiveRoot = Join-Path $workspace "PSP-Archive-Resources"
$pspMoonlightRoot = Join-Path $workspace "psp_moonlight"

# Check if directories exist
if (-not (Test-Path $pspArchiveRoot)) {
    Write-Error "Directory not found: $pspArchiveRoot"
    exit 1
}
if (-not (Test-Path $pspMoonlightRoot)) {
    Write-Error "Directory not found: $pspMoonlightRoot"
    exit 1
}

# Get all files recursively in each directory with relative paths
Write-Host "Scanning PSP-Archive-Resources..." -ForegroundColor Cyan
$pspArchiveFiles = Get-ChildItem -Path $pspArchiveRoot -File -Recurse | Select-Object -ExpandProperty FullName | ForEach-Object { 
    $_.Substring($pspArchiveRoot.Length + 1) 
}
Write-Host "Scanning psp_moonlight..." -ForegroundColor Cyan
$pspMoonlightFiles = Get-ChildItem -Path $pspMoonlightRoot -File -Recurse | Select-Object -ExpandProperty FullName | ForEach-Object { 
    $_.Substring($pspMoonlightRoot.Length + 1) 
}

# Find files only in each directory
$onlyInPSPArchive = $pspArchiveFiles | Where-Object { -not ($pspMoonlightFiles -contains $_) }
$onlyInPSPMoonlight = $pspMoonlightFiles | Where-Object { -not ($pspArchiveFiles -contains $_) }
$commonFiles = $pspArchiveFiles | Where-Object { $pspMoonlightFiles -contains $_ }

Write-Host "Found $($pspArchiveFiles.Count) files in PSP-Archive-Resources"
Write-Host "Found $($pspMoonlightFiles.Count) files in psp_moonlight"
Write-Host "Only in PSP-Archive-Resources: $($onlyInPSPArchive.Count)"
Write-Host "Only in psp_moonlight: $($onlyInPSPMoonlight.Count)"
Write-Host "Common files: $($commonFiles.Count)"

# Initialize result collections
$identical = @()
$different = @()

# Compare common files
foreach ($relativePath in $commonFiles) {
    $pspArchiveFile = Join-Path $pspArchiveRoot $relativePath
    $pspMoonlightFile = Join-Path $pspMoonlightRoot $relativePath
    
    try {
        # Read both files as byte arrays
        $bytes1 = [System.IO.File]::ReadAllBytes($pspArchiveFile)
        $bytes2 = [System.IO.File]::ReadAllBytes($pspMoonlightFile)
        
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
    onlyInPSPArchive = $onlyInPSPArchive | Sort-Object
    onlyInPSPMoonlight = $onlyInPSPMoonlight | Sort-Object
    identical = $identical | Sort-Object
    different = $different | Sort-Object Path
}

# Convert to JSON and output
$json = $result | ConvertTo-Json -Depth 5
Write-Output $json
