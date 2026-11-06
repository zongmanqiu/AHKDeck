# lint_bats.ps1 - static-check .bat files for cmd's known parsing traps
# BEFORE running them. Every finding here is a bug that previously failed at
# runtime with a cryptic cmd error (m006 first-day lessons).
# Usage: powershell -File lint_bats.ps1 <file.bat | dir> [more paths...]
# Exit 0 = clean, 1 = findings (prints each as FILE:LINE: FINDING).

param([Parameter(Mandatory=$true)][string[]]$Paths)

$files = @()
foreach ($p in $Paths) {
    if (Test-Path $p -PathType Leaf) { $files += $p }
    elseif (Test-Path $p -PathType Container) { $files += (Get-ChildItem $p -Filter *.bat -Recurse | ForEach-Object { $_.FullName }) }
}

$findings = New-Object System.Collections.Generic.List[string]

function Add-Finding($file, $line, $msg) {
    $script:findings.Add("$file`:$line`: $msg")
}

foreach ($f in $files) {
    $bytes = [IO.File]::ReadAllBytes($f)
    # F1: .bat must be pure ASCII (cmd parses in OEM code page)
    $nonAscii = 0
    foreach ($b in $bytes) { if ($b -gt 127) { $nonAscii++ } }
    if ($nonAscii -gt 0) { Add-Finding $f 0 "non-ASCII bytes: $nonAscii (cmd OEM code page trap)" }

    $text = [Text.Encoding]::ASCII.GetString($bytes)
    # F2: LF-only line endings
    $cr = ([regex]::Matches($text, "`r`n")).Count
    $lf = ([regex]::Matches($text, "`n")).Count
    if ($cr -ne $lf) { Add-Finding $f 0 "LF-only line endings detected (need CRLF)" }

    # F3: track ( ) block depth; inside a block, unescaped parens in REM/echo
    # lines close the block prematurely ("... was unexpected at this time.")
    $lines = $text -split "`r`n"
    $depth = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].TrimEnd()
        $code = ($line -replace '\^.', '')           # drop escaped chars
        $code = ($code -replace '"[^"]*"', '""')      # drop quoted strings
        $trimmed = $code.TrimStart()
        $isRem = $trimmed -match '^(REM|::)(\s|$)'
        $isEcho = $trimmed -match '^(echo|echo\.)(\s|\.|$)'

        if ($depth -gt 0 -and ($isRem -or $isEcho)) {
            if ($code -match '[()]') {
                Add-Finding $f ($i + 1) "unescaped paren inside a block on a $(if ($isRem) {'REM'} else {'echo'}) line: $($line.Trim())"
            }
        }
        # depth tracking: opens on trailing ( , closes on a bare ) line
        $opens  = ([regex]::Matches($code, '\(')).Count
        $closes = ([regex]::Matches($code, '\)')).Count
        if ($isEcho -or $isRem) { continue }  # REM/echo don't drive depth (their parens are the trap, flagged above)
        $depth += $opens - $closes
        if ($depth -lt 0) { $depth = 0 }
    }

    # F4: echo lines with unescaped redirection (echo X -> path truncates path!)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $trimmed = $lines[$i].TrimStart()
        if ($trimmed -match '^(REM|::)(\s|$)') { continue }
        if ($trimmed -match '^(echo|echo\.)(\s|\.|$)') {
            $unq = $trimmed -replace '"[^"]*"', '""'
            $unq = $unq -replace '\^.', ''
            if ($unq -match '(?<!\^)[<>]') {
                Add-Finding $f ($i + 1) "unescaped < or > in echo line (redirects/truncates a file): $($trimmed)"
            }
        }
    }

    # F5: cd /d "%VAR%\" - %~dp0-style var ends with backslash, the \" eats the quote
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'cd /d "%[^"]*%\\"') {
            Add-Finding $f ($i + 1) 'cd into quoted path ending with backslash (trailing \" escape trap) - use "%VAR%."'
        }
    }
}

if ($findings.Count -gt 0) {
    $findings | ForEach-Object { Write-Output $_ }
    Write-Output "LINT: $($findings.Count) finding(s)"
    exit 1
}
Write-Output "LINT: clean ($($files.Count) file(s))"
exit 0
