"""Lanza XenonRecomp con config/gowj.toml y resume el log. Uso: python tools/run_recomp.py [log]"""
import subprocess, sys, os, re, collections
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "logs", "recomp.log")
exe = os.path.join(root, "XenonRecomp", "build-msvc", "XenonRecomp", "XenonRecomp.exe")
import time, glob, shutil
t0 = time.time() - 2
p = subprocess.run([exe, "gowj.toml", os.path.join(root, "XenonRecomp", "XenonUtils", "ppc_context.h")],
                   cwd=os.path.join(root, "config"), capture_output=True, text=True)
out = p.stdout + p.stderr
open(log, "w").write(out)
print("rc", p.returncode)
# restos de una generacion anterior con mas trozos: indices mayores que el del trozo que define la ultima funcion del mapa
# (XenonRecomp no reescribe los trozos que no cambian, asi que la fecha no sirve)
stale_dir = os.path.join(root, "ppc_viejos"); moved = 0
if p.returncode == 0:
    last = re.findall(r"\{ 0x\w+, (\w+) \}", open(os.path.join(root, "ppc", "ppc_func_mapping.cpp")).read())[-1]
    files = glob.glob(os.path.join(root, "ppc", "ppc_recomp.*.cpp"))
    idx = lambda f: int(os.path.basename(f).split(".")[1])
    maxi = max(idx(f) for f in files if ("PPC_FUNC_IMPL(__imp__%s)" % last) in open(f).read())
    for f in files:
        if idx(f) > maxi:
            os.makedirs(stale_dir, exist_ok=True); shutil.move(f, os.path.join(stale_dir, os.path.basename(f))); moved += 1
    # GoWJ: trozo viejo cuyas funciones estan todas en otros trozos (la deteccion por indice falla si el viejo tiene la ultima)
    own = collections.defaultdict(list)
    for f in glob.glob(os.path.join(root, "ppc", "ppc_recomp.*.cpp")):
        for m in re.findall(r"PPC_FUNC_IMPL\(__imp__(\w+)\)", open(f).read()): own[m].append(f)
    dupf = collections.Counter(f for k, v in own.items() if len(v) > 1 for f in v)
    for f in sorted(dupf, key=os.path.getmtime)[:1] if dupf else []:
        os.makedirs(stale_dir, exist_ok=True); shutil.move(f, os.path.join(stale_dir, os.path.basename(f))); moved += 1
if moved: print("apartados", moved, "archivos viejos en ppc_viejos/")
sw = sorted(set(re.findall(r"Switch case at (\w+)", out)))
print("switches con error:", len(sw), " ".join(sw[:10]))
un = collections.Counter(re.findall(r"Unrecognized instruction at \w+: (\S+)", out))
print("instrucciones sin reconocer:", sum(un.values()), dict(un.most_common()))
rc = collections.Counter(re.findall(r"(\S+) at \w+ has RC bit", out))
print("RC sin comparacion:", dict(rc))
other = [l for l in out.splitlines() if not re.search(r"Switch case|Unrecognized|Recompiling|RC bit", l)]
print("otras lineas:", len(other)); print("\n".join(other[:15]))
