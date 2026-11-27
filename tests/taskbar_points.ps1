Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AuraTestTaskbar {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
}
'@

[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[AuraTestTaskbar]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
$root = [System.Windows.Automation.AutomationElement]::FromHandle([AuraTestTaskbar]::FindWindow('Shell_TrayWnd',$null))
$nodes = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$result = @{}
foreach ($node in $nodes) {
    try {
        $item = $node.Current
        $key = $null
        if ($item.AutomationId -match '^Appid: FullWinTB\.Regression\.(main|peer-one)$') { $key = $Matches[1] }
        if ($item.AutomationId -eq 'NotifyItemIcon' -and $item.Name -match '^(微信|WeChat|Weixin)(\W|$)') { $key = 'weixin' }
        if (!$key -or $item.BoundingRectangle.IsEmpty) { continue }
        $box = $item.BoundingRectangle
        $result[$key] = @([int]($box.Left + $box.Width/2), [int]($box.Top + $box.Height/2))
    } catch {}
}
$result | ConvertTo-Json -Compress
