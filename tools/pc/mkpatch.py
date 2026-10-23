# tools/pc/mkpatch.py <src rel path> <repl.py>: original + existing host patch + REPL -> new host patch
import sys,subprocess,os,runpy,shutil
rel=sys.argv[1]; spec=runpy.run_path(sys.argv[2])['REPL']
pp='pc/host/patches/'+rel+'.patch'
import tempfile; tmpd=tempfile.mkdtemp(prefix='mkpatch-')
pass
work=tmpd+'/f.c'; shutil.copy(rel,work)
if os.path.exists(pp):
    r=subprocess.run(['patch','-s','-o',work+'.p',rel,pp],capture_output=True,text=True)
    assert r.returncode==0,r.stdout+r.stderr
    shutil.move(work+'.p',work)
s=open(work).read()
for old,new in spec:
    assert s.count(old)==1,(rel,old[:80],s.count(old))
    s=s.replace(old,new)
open(work,'w').write(s)
os.makedirs(os.path.dirname(pp),exist_ok=True)
d=subprocess.run(['diff','-u','--label','a/'+rel,'--label','b/'+rel,rel,work],capture_output=True,text=True).stdout
open(pp,'w').write(d); print(pp, d.count('\n'),'lines')
