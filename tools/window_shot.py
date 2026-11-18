"""Captura la ventana real de NG2 (no la imagen interna) tras SEGUNDOS (por defecto 52).
Uso: python tools/window_shot.py salida.png [VAR=valor ...]   p. ej. NG2_INPUTSCRIPT=..\scripts\menu13b.txt"""
import subprocess, time, sys, os
root = "G:/Gears of war Judgmanet pc"
env = dict(os.environ)
for a in sys.argv[2:]: k, v = a.split("=", 1); env[k] = v
exe = os.path.join(root, "project", "build", "GOWJ.exe")
N = "Gears of War - Judgment (USA, Europe) (En,Fr,Es,Nl,No,Da,Fi,Zh,Ko,Pl,Ru,Cs)"
iso = os.path.join("G:/gears of war judgment", N, N + ".iso")
p = subprocess.Popen([exe, os.path.join(root, "xex", "basefile.bin"), os.path.join(root, "xex", "default.xex"), iso, "userdata"], cwd=os.path.join(root, "run"), stdout=subprocess.DEVNULL, stderr=open(os.path.join(root, 'logs', 'window_shot.log'), 'w'), env=env)
time.sleep(float(os.environ.get("SEGUNDOS", "52")))
ps = r'''Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct RECT { public int L, T, R, B; } }
"@
$h = (Get-Process -Id PID).MainWindowHandle; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300; $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
$b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size); $b.Save("OUT")'''
subprocess.run(["powershell", "-NoProfile", "-Command", ps.replace("PID", str(p.pid)).replace("OUT", sys.argv[1])], capture_output=True)
p.kill()
