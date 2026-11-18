"""Sesion 9: captura la ventana del launcher (GOWJ.exe --launcher en modo usuario). python tools/launcher_shot.py salida.png [pagina]"""
import subprocess, time, sys, os
root = "G:/Gears of war Judgmanet pc"
env = dict(os.environ)
if len(sys.argv) > 2: env["GOWJ_LAUNCHER_PAGE"] = sys.argv[2]
exe = os.path.join(root, "project", "build", "GOWJ.exe")
p = subprocess.Popen([exe, "--launcher"], cwd=os.path.dirname(exe), env=env)
time.sleep(3)
ps = r'''Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct RECT { public int L, T, R, B; } }
"@
[W]::SetProcessDPIAware() | Out-Null; $h = (Get-Process -Id PID).MainWindowHandle; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 400; $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
$b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size); $b.Save("OUT")'''
subprocess.run(["powershell", "-NoProfile", "-Command", ps.replace("PID", str(p.pid)).replace("OUT", sys.argv[1])], capture_output=True)
p.kill()
