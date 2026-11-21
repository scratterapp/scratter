# Creates a Start Menu shortcut. `cmake --install` runs it on Windows (see
# CMakeLists.txt next to this file); it isn't meant to be run by hand, though it
# can be:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File make-shortcut.ps1 `
#       -Shortcut "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Scratter.lnk" `
#       -Target "$env:LOCALAPPDATA\Programs\Scratter\am-ui.exe" -Description "..."
#
# The icon is the target's own (am-ui.rc puts it in am-ui.exe), and the program
# starts in its own folder.

param(
    [Parameter(Mandatory)] [string] $Shortcut,
    [Parameter(Mandatory)] [string] $Target,
    [string] $Description = ''
)

$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force (Split-Path $Shortcut) | Out-Null
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($Shortcut)
$link.TargetPath = $Target
$link.WorkingDirectory = Split-Path $Target
$link.IconLocation = "$Target,0"
$link.Description = $Description
$link.Save()
