<#
.SYNOPSIS
    Reads every control of the configuration utility the way a screen reader would.

.DESCRIPTION
    The companion to tools\check_installer_a11y.ps1, and for the same reason: the people
    most likely to want these voices are the ones least able to work around a control that
    announces itself as nothing but its type.

        powershell -ExecutionPolicy Bypass -File tools\check_config_a11y.ps1

    Every control on every tab page is queried through MSAA (oleacc) rather than UI
    Automation, because PowerShell's UIA client reports most Win32 controls as a generic
    pane. oleacc reports what NVDA and JAWS see.

    A control is flagged when it is focusable and enabled but has no accessible name. In a
    Win32 dialog a control takes its name from the static text before it in the tab order,
    so a flagged control almost always means a label is missing or is in the wrong place in
    the template.

    The utility is closed with Cancel, so nothing is written.
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build_x86\bin\Release\Infovox330Config.exe'),
    [string]$DataDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bin')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "the configuration utility was not found: $Exe" }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Acc
{
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppv);

    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);

    public static readonly Guid IID_IDispatch = new Guid("00020400-0000-0000-C000-000000000046");
    public const uint OBJID_CLIENT = 0xFFFFFFFC;

    // The top-level dialog of the process just started. Matching on the process rather than
    // the title avoids finding a stale window from an earlier run.
    public static IntPtr FindDialogOfProcess(uint wanted)
    {
        IntPtr hit = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != wanted) return true;
            if (ClassOf(h) != "#32770") return true;
            hit = h;
            return false;
        }, IntPtr.Zero);
        return hit;
    }

    public static List<IntPtr> Descendants(IntPtr parent)
    {
        var found = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }

    public static string ClassOf(IntPtr hwnd)
    {
        var sb = new StringBuilder(256);
        GetClassNameW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string TextOf(IntPtr hwnd)
    {
        var sb = new StringBuilder(1024);
        GetWindowTextW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static object Accessible(IntPtr hwnd)
    {
        object acc;
        Guid iid = IID_IDispatch;
        if (AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, ref iid, out acc) == 0) return acc;
        return null;
    }
}
'@

$roleNames = @{
    3  = 'scroll bar';   9  = 'window';       10 = 'client';        20 = 'grouping'
    21 = 'separator';    33 = 'list';         34 = 'list item';     35 = 'outline'
    36 = 'outline item'; 37 = 'page tab';     38 = 'property page'; 41 = 'static text'
    42 = 'editable text'; 43 = 'push button'; 44 = 'check box';     45 = 'radio button'
    46 = 'combo box';    47 = 'drop list';    48 = 'progress bar';  51 = 'slider'
    52 = 'spin box';     60 = 'page tab list'
}

# Containers legitimately have no name of their own; a screen reader never lands on them.
$containerRoles = @(9, 10, 20, 21, 38)

$STATE_INVISIBLE   = 0x8000
$STATE_FOCUSABLE   = 0x100000
$STATE_UNAVAILABLE = 0x1

function Get-AccName($acc) { try { return [string]$acc.accName(0) } catch { return '' } }
function Get-AccRole($acc) { try { return [int]$acc.accRole(0) } catch { return -1 } }
function Get-AccState($acc) { try { return [int]$acc.accState(0) } catch { return 0 } }

Get-Process -Name 'Infovox330Config' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$env:INFOVOX330_DATA_DIR = $DataDir
Write-Host "Launching $Exe"
$proc = Start-Process -FilePath $Exe -PassThru

$deadline = (Get-Date).AddSeconds(20)
$hwnd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    $hwnd = [Acc]::FindDialogOfProcess($proc.Id)
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
}
if ($hwnd -eq [IntPtr]::Zero) {
    if (-not $proc.HasExited) { $proc | Stop-Process -Force }
    throw 'the configuration window never appeared'
}

Write-Host "Window: '$([Acc]::TextOf($hwnd))'"
Write-Host ""

$IDC_TABS = 1000
$TCM_SETCURFOCUS = 0x1330    # TCM_FIRST + 48; unlike TCM_SETCURSEL this notifies the parent
$tabs = [Acc]::GetDlgItem($hwnd, $IDC_TABS)

$unnamed = 0
$total = 0
$pageNames = @('Voices', 'Text and reporting', 'Speech engine')

for ($page = 0; $page -lt 3; $page++) {
    if ($tabs -ne [IntPtr]::Zero) {
        [void][Acc]::SendMessageW($tabs, $TCM_SETCURFOCUS, [IntPtr]$page, [IntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 500

    Write-Host "--- Page $($page + 1): $($pageNames[$page])" -ForegroundColor Cyan
    foreach ($child in [Acc]::Descendants($hwnd)) {
        if (-not [Acc]::IsWindowVisible($child)) { continue }
        $acc = [Acc]::Accessible($child)
        if ($null -eq $acc) { continue }
        $state = Get-AccState $acc
        if ($state -band $STATE_INVISIBLE) { continue }

        $role = Get-AccRole $acc
        $name = Get-AccName $acc
        if ([string]::IsNullOrWhiteSpace($name)) { $name = [Acc]::TextOf($child) }
        $name = ($name -replace '\s+', ' ').Trim()

        $focusable = [bool]($state -band $STATE_FOCUSABLE)
        $enabled = -not [bool]($state -band $STATE_UNAVAILABLE)
        $cls = [Acc]::ClassOf($child)
        $roleName = if ($roleNames.ContainsKey($role)) { $roleNames[$role] } else { "role $role" }

        $total++
        $flag = ''
        # Deliberately not excused for being disabled: a control that is unnamed while
        # greyed out is unnamed when it is switched on, and that is how the real time value
        # went unreported the first time this ran.
        if ($focusable -and -not $name -and $containerRoles -notcontains $role) {
            $flag = '   <== FOCUSABLE WITH NO ACCESSIBLE NAME'
            $unnamed++
        }
        $marker = if (-not $focusable) { '     ' } elseif ($enabled) { '[tab]' } else { '[off]' }
        $short = if ($name.Length -gt 76) { $name.Substring(0, 73) + '...' } else { $name }
        Write-Host ("    {0} {1,-17} {2,-14} {3}{4}" -f $marker, $cls, $roleName, $short, $flag)
    }
    Write-Host ""
}

# Cancel, so the run never writes a settings file. Nothing was changed, so no prompt appears.
[void][Acc]::SendMessageW($hwnd, 0x0111, [IntPtr]2, [IntPtr]::Zero)   # WM_COMMAND, IDCANCEL
Start-Sleep -Milliseconds 700
if (-not $proc.HasExited) { $proc | Stop-Process -Force -ErrorAction SilentlyContinue }

Write-Host "================================================================"
Write-Host "$total visible controls across 3 pages, $unnamed focusable without a name"
if ($unnamed -eq 0) {
    Write-Host "Every focusable control reports a name to MSAA." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Some controls would be announced as their type only." -ForegroundColor Red
    exit 1
}
