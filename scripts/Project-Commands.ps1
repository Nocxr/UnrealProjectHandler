param(
    [Parameter(Mandatory)][ValidateSet('Stop','Run','Clean','Install','Uninstall')][string]$Action,
    [string]$Executable, [string]$BuildDir, [string]$CommandName,
    [string]$ProjectName, [string]$RunArguments, [string]$ExtraExecutables
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
function FullPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}
function CommandPath {
    if ($CommandName -notmatch '^[A-Za-z0-9_-]+$') { throw 'Invalid command name.' }
    return Join-Path (Join-Path $env:LOCALAPPDATA 'CxxTools\bin') ($CommandName + '.cmd')
}
switch ($Action) {
    'Stop' {
        $paths = @($Executable) + @($ExtraExecutables -split ';' | Where-Object { $_ })
        foreach ($candidate in $paths) {
            if (-not $candidate) { continue }
            $target = FullPath $candidate
            Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
                try { $processPath = $_.Path } catch { $processPath = $null }
                if ($processPath -and [string]::Equals($processPath, $target, [StringComparison]::OrdinalIgnoreCase)) {
                    Stop-Process -Id $_.Id -Force -ErrorAction Stop
                }
            }
        }
    }
    'Run' {
        $target = FullPath $Executable
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Executable missing: $target" }
        $launch = @{FilePath=$target; WorkingDirectory=$projectRoot; WindowStyle='Hidden'}
        if ($RunArguments) { $launch.ArgumentList = $RunArguments }
        Start-Process @launch
    }
    'Clean' {
        $target = FullPath $BuildDir
        $rootPrefix = $projectRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        if (-not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to delete a build directory outside the project: $target"
        }
        if ((Split-Path -Leaf $target) -in @('.git','src','scripts','include','assets','third_party')) {
            throw "Refusing to delete a source directory: $target"
        }
        if (Test-Path -LiteralPath $target) {
            if ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Refusing to recursively delete a junction or symlink.'
            }
            $links = Get-ChildItem -LiteralPath $target -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
            if ($links) { throw 'Build directory contains junctions or symlinks; remove them explicitly first.' }
            Remove-Item -LiteralPath $target -Recurse -Force
        }
    }
    'Install' {
        $target = FullPath $Executable
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Executable missing: $target" }
        if ($target -match '[%"\r\n]' -or $projectRoot -match '[%"\r\n]') { throw 'Path cannot be represented safely by a CMD launcher.' }
        $shim = CommandPath
        if ((Test-Path -LiteralPath $shim) -and (Get-Content -LiteralPath $shim -Raw) -notmatch 'CxxTools managed launcher') {
            throw "Refusing to overwrite an unmanaged command: $shim"
        }
        $shimDir = Split-Path -Parent $shim
        New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
        $body = "@echo off`r`nrem CxxTools managed launcher`r`npushd `"$projectRoot`"`r`n`"$target`" %*`r`nset `"CXX_EXIT=%ERRORLEVEL%`"`r`npopd`r`nexit /b %CXX_EXIT%`r`n"
        [IO.File]::WriteAllText($shim, $body, [Text.UTF8Encoding]::new($false))
        $userPath = [Environment]::GetEnvironmentVariable('Path','User')
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $kept = [Collections.Generic.List[string]]::new()
        $escapedProject = [regex]::Escape($ProjectName)
        foreach ($entry in @($userPath -split ';' | Where-Object { $_ })) {
            $normal = [Environment]::ExpandEnvironmentVariables($entry.Trim().Trim('"')).TrimEnd('\','/')
            if ($normal -match "(?i)[\\/]$escapedProject[\\/]build(?:-[^\\/]*)?$" -and $normal -ne $shimDir) { continue }
            if ($seen.Add($normal)) { $kept.Add($entry) }
        }
        if ($seen.Add($shimDir)) { $kept.Add($shimDir) }
        [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
        Write-Host "Registered $CommandName -> $target. Open a new terminal. Run make install again after moving this checkout."
    }
    'Uninstall' {
        $shim = CommandPath
        if (Test-Path -LiteralPath $shim) {
            if ((Get-Content -LiteralPath $shim -Raw) -notmatch 'CxxTools managed launcher') { throw 'Refusing to remove an unmanaged command.' }
            Remove-Item -LiteralPath $shim
        }
        Write-Host "Removed $CommandName launcher. Shared PATH entry is retained for other tools."
    }
}