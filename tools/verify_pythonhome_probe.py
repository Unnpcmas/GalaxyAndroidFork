import sys, os, json, importlib.abc
# -I ignores env; read BLOCK from the launcher via a side channel
blocked = set(json.loads(os.environ.get("BLOCK","[]"))) if "BLOCK" in os.environ else set()
class Block(importlib.abc.MetaPathFinder):
    def find_spec(self, name, path=None, target=None):
        if name in blocked: raise ModuleNotFoundError("blocked: "+name, name=name)
sys.meta_path.insert(0, Block())
print("python", sys.version.split()[0], "| stdlib:", os.path.dirname(os.__file__))
sys.path.insert(0, sys.argv[1])
import cook, validate, rarc, thp, j3d, jaudio, kcl, bmg, bcsv, lyt, jpa, dol_data, collision, rarc_swap, util
import traceback, argparse, concurrent.futures, mmap, ctypes, shutil, collections, math, struct, time, multiprocessing
assert 'シ'.encode('shift_jis').decode('shift_jis') == 'シ'
b'\x82\xa0'.decode('shift_jis'); 'é'.encode('latin-1'); 'x'.encode('utf-8')
with concurrent.futures.ThreadPoolExecutor(4) as ex: assert list(ex.map(lambda x: x*2, range(8))) == [x*2 for x in range(8)]
import tempfile
d = tempfile.mkdtemp(); a = os.path.join(d,'a'); b = os.path.join(d,'b'); open(a,'wb').write(b'hello'*1000)
shutil.copyfile(a, b); assert open(b,'rb').read() == b'hello'*1000
with open(a,'rb') as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as mm: assert mm[:5] == b'hello'
c = collections.Counter('abca'); assert c['a'] == 2
# Yaz0 pure-python path
import rarc as R
raw = b'Yaz0' + struct.pack('>I', 8) + b'\0'*8 + bytes([0xFF]) + b'ABCDEFGH'
assert R.yaz0_decompress(raw) == b'ABCDEFGH', R.yaz0_decompress(raw)
# the exact error path used by py_bridge.cpp
try:
    cook.convert(data_dir=d, out_dir=os.path.join(d,'o'), with_movies=False, progress=lambda *a: None)
except BaseException as e:
    s = traceback.format_exc(); assert 'convert' in s
# dropped extension really is absent, and the try/except consumers still import
for m in ("_zstd","_lzma","_bz2","_ssl","_sqlite3"):
    try: __import__(m); raise SystemExit("not blocked: "+m)
    except ModuleNotFoundError: pass
import random, hashlib, hmac, pickle, json as _j, re, logging, secrets
hashlib.sha256(b'x').hexdigest(); hashlib.md5(b'x').hexdigest(); random.random(); re.compile(r'\N{LATIN SMALL LETTER A}')
loaded = sorted(m for m in sys.modules if m.split('.')[0] in ('pdb','pydoc','doctest','unittest','asyncio','email','http','urllib','xml','sqlite3','ssl'))
print("heavy modules loaded:", loaded)
print("modules loaded total:", len(sys.modules))
print("OK")
