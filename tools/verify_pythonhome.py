#!/usr/bin/env python3.15
"""Check a trimmed pythonhome.zip against a real CPython 3.15 (host build).

Builds a private prefix from the host stdlib that contains ONLY the .py files the
trimmed zip keeps (same relative paths), blocks the C extensions the trim drops,
then runs the cooker import chain + error path inside it.
    python3.15 verify_pythonhome.py TRIMMED.zip SCRIPTS_DIR
"""
import os, re, shutil, subprocess, sys, zipfile, tempfile, json
trim, scripts = sys.argv[1], sys.argv[2]
P = "lib/python3.15/"
kept = {i.filename[len(P):] for i in zipfile.ZipFile(trim).infolist() if i.filename.startswith(P) and not i.filename.endswith("/")}
host_std = os.path.dirname(os.__file__)
pfx = tempfile.mkdtemp(prefix="pfx_")
dst = os.path.join(pfx, "lib", "python3.15"); os.makedirs(dst)
os.makedirs(os.path.join(pfx, "bin"))
shutil.copy2(sys.executable, os.path.join(pfx, "bin", "python3.15"))
n = missing = 0
for root, _, files in os.walk(host_std):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), host_std)
        if rel.endswith(".py") and "site-packages" not in rel and not rel.startswith("lib-dynload"):
            if rel in kept or rel.startswith("_sysconfigdata__linux"):
                os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
                shutil.copy2(os.path.join(root, f), os.path.join(dst, rel)); n += 1
for rel in kept:
    if rel.endswith(".py") and not os.path.exists(os.path.join(dst, rel)): missing += 1
print("prefix:", pfx, "| py files copied:", n, "| kept-in-zip but absent on host (android-only):", missing)
# extension modules removed from the APK zip
dropped_ext = ["_zstd","_lzma","_bz2","_ssl","_hashlib","_sqlite3","_codecs_cn","_codecs_hk","_codecs_kr","_codecs_tw",
               "_remote_debugging","_lsprof","_interpchannels","_testcapi","_testinternalcapi","_ctypes_test","_xxtestfuzz"]
probe = open(os.path.join(os.path.dirname(__file__), "verify_pythonhome_probe.py")).read()
env = {"BLOCK": json.dumps(dropped_ext), "PATH": os.environ["PATH"]}
r = subprocess.run([os.path.join(pfx, "bin", "python3.15"), "-I", "-c", probe, scripts], capture_output=True, text=True, env=env)
print(r.stdout[-3000:]); print(r.stderr[-3000:])
print("RESULT:", "PASS" if r.returncode == 0 else "FAIL(%d)" % r.returncode)
shutil.rmtree(pfx, ignore_errors=True)
sys.exit(r.returncode)
