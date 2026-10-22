#!/usr/bin/env python3
"""Shrink installer-mobile/vendor/python/stdlib/pythonhome.zip.

The embedded CPython only runs the disc cooker (installer-mobile/assets/scripts),
so most of the standard library never loads.  This keeps the static import
closure of the modules the cooker (and interpreter start-up) really use, plus
every encoding codec and the Android/sysconfig support files, and drops the rest
(tests, pydoc, profiling, unittest, tkinter-era leftovers, xml/http/email...,
unused C extensions).  Anything not provably needed stays out; anything doubtful
stays in.

    python3 tools/trim_pythonhome.py IN.zip OUT.zip [--report]

Verify afterwards with tools/verify_pythonhome.py.
"""
import ast
import re
import sys
import zipfile

PREFIX = "lib/python3.15/"

# Modules the cooker and interpreter start-up import (found by tracing the real
# cook.py import chain on CPython 3.15), plus Android start-up support.
ROOTS = """
__future__ _collections_abc _colorize _compat_pickle _py_warnings _sitebuiltins _weakrefset abc
annotationlib argparse ast bisect bz2 codecs codeop collections compression compression._common
compression._common._streams compression.zstd compression.zstd._zstdfile concurrent concurrent.futures
concurrent.futures._base concurrent.futures.thread contextlib copyreg ctypes ctypes._endian dataclasses
encodings encodings.aliases encodings.shift_jis encodings.utf_8 enum fnmatch functools genericpath
gettext heapq importlib importlib._abc importlib._bootstrap importlib._bootstrap_external importlib.util
io json json.decoder json.encoder json.scanner keyword linecache logging lzma multiprocessing
multiprocessing.context multiprocessing.process multiprocessing.reduction ntpath operator os pathlib
pathlib._os pickle posixpath queue random re re._casefix re._compiler re._constants re._parser reprlib
shutil signal site socket stat string struct sysconfig tempfile textwrap threading token tokenize
traceback types warnings weakref zipimport
_android_support _sysconfigdata__android_aarch64-linux-android
""".split()

# Import edges that only fire in interactive/archive code paths the embedded
# cooker never runs (help(), doctest self-test, the REPL, shutil archive helpers).
# The child modules are simply not pulled in through these edges.
STOP_EDGES = [
    ("_sitebuiltins.py", r"pydoc"),
    ("heapq.py", r"doctest"),
    ("site.py", r"(_pyrepl|rlcompleter)"),
    ("shutil.py", r"(tarfile|zipfile)"),
]

# Always kept: licence, build metadata, sysconfig data, the config dir (tiny).
KEEP_PATTERNS = [
    r"^LICENSE", r"^build-details\.json$", r"^_sysconfig", r"^_android", r"^config-",
    r"^site-packages/", r".*\.pth$", r"^sitecustomize\.py$", r"^usercustomize\.py$",
]

# C extensions that are safe to drop: test/demo modules, debugging/profiling
# tooling, and everything that would need libssl/libcrypto/libsqlite (nothing in
# the cooker imports ssl, hashlib's OpenSSL backend or sqlite3; hashlib falls
# back to the built-in _md5/_sha*/_blake2 modules).  _zstd/_lzma/_bz2 are only
# imported by shutil inside try/except ImportError (archive helpers).  Shift-JIS (_codecs_jp,
# _multibytecodec) is used by the cooker and stays.
DROP_EXT = re.compile(
    r"^lib-dynload/(_test\w*|_xxtestfuzz|_ctypes_test|xx\w+|_remote_debugging|_lsprof|"
    r"_interpchannels|_ssl|_hashlib|_sqlite3|_zstd|_lzma|_bz2|_codecs_cn|_codecs_hk|_codecs_kr|_codecs_tw)\.cpython-"
)
# Codec modules whose C backend was dropped above.
DROP_CODECS = re.compile(
    r"^encodings/(big5|big5hkscs|cp949|cp950|euc_kr|gb2312|gbk|gb18030|hz|johab|iso2022_kr)\.py$"
)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    zin = zipfile.ZipFile(src)
    infos = {i.filename[len(PREFIX):]: i for i in zin.infolist()
             if i.filename.startswith(PREFIX) and not i.filename.endswith("/")}

    def path_of(mod):
        p = mod.replace(".", "/")
        for c in (p + ".py", p + "/__init__.py"):
            if c in infos:
                return c
        return None

    def modname(rel):
        return (rel[:-12] if rel.endswith("/__init__.py") else rel[:-3]).replace("/", ".")

    seen, stack = set(), []
    for r in ROOTS:
        p = path_of(r)
        if p:
            stack.append(p)
        else:
            print("note: root not found in zip:", r, file=sys.stderr)
    while stack:
        rel = stack.pop()
        if rel in seen:
            continue
        seen.add(rel)
        tree = ast.parse(zin.read(PREFIX + rel).decode("utf-8"))
        pkg = modname(rel) if rel.endswith("__init__.py") else modname(rel).rpartition(".")[0]
        for n in ast.walk(tree):
            names = []
            if isinstance(n, ast.Import):
                names = [a.name for a in n.names]
            elif isinstance(n, ast.ImportFrom):
                base = n.module or ""
                if n.level:
                    parts = pkg.split(".") if pkg else []
                    if n.level > 1:
                        parts = parts[:len(parts) - (n.level - 1)]
                    base = ".".join(parts + ([base] if base else []))
                names = [base] + [base + "." + a.name for a in n.names]
            for nm in names:
                if any(rel == pr and re.match(ch + r"(\.|$)", nm) for pr, ch in STOP_EDGES):
                    continue
                parts = nm.split(".")
                for i in range(1, len(parts) + 1):
                    p = path_of(".".join(parts[:i]))
                    if p and p not in seen:
                        stack.append(p)

    keep, dropped = [], []
    for rel, info in infos.items():
        if rel.startswith("encodings/") and rel.endswith(".py"):
            ok = not DROP_CODECS.match(rel)
        elif rel.startswith("lib-dynload/") and rel.endswith(".so"):
            ok = not DROP_EXT.match(rel)
        elif rel.endswith(".py") and rel in seen:
            ok = True
        else:
            ok = any(re.match(p, rel) for p in KEEP_PATTERNS)
        (keep if ok else dropped).append(info)

    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zout:
        for info in sorted(keep, key=lambda i: i.filename):
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = info.external_attr
            zout.writestr(zi, zin.read(info.filename), compresslevel=9)

    def tot(lst, attr):
        return sum(getattr(i, attr) for i in lst)
    print("kept   %4d files  %9d B raw  %9d B zipped" % (len(keep), tot(keep, "file_size"), tot(keep, "compress_size")))
    print("dropped%4d files  %9d B raw  %9d B zipped" % (len(dropped), tot(dropped, "file_size"), tot(dropped, "compress_size")))
    if "--report" in sys.argv:
        for i in sorted(keep, key=lambda i: i.filename):
            print("KEEP", i.filename[len(PREFIX):])


if __name__ == "__main__":
    main()
