import re,pathlib,collections,sys
ins=collections.Counter(); examples={}
strmac={}
files=list(pathlib.Path('include').rglob('*.h'))+list(pathlib.Path('src').rglob('*.[ch]'))+list(pathlib.Path('src').rglob('*.inc'))
files=[f for f in files if '/psyq/' not in str(f)] if '--no-psyq' in sys.argv else files
for p in files:
    s=p.read_text(errors='ignore')
    for m in re.finditer(r'#define\s+(\w+)\s+("(?:[^"\\]|\\.)*")\s*$',s,re.M): strmac[m.group(1)]=m.group(2)
for p in files:
    s=p.read_text(errors='ignore')
    for m in re.finditer(r'\b(?:asm|__asm__)\s*(?:volatile|__volatile__)?\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*|\w+\s*)+)',s):
        t=m.group(1); txt=''
        for mm in re.finditer(r'"((?:[^"\\]|\\.)*)"|(\w+)',t):
            a,b=mm.group(1),mm.group(2)
            if a is not None: txt+=a
            elif b in strmac: txt+=strmac[b][1:-1]
            else: txt+='<'+b+'>'
        for line in re.split(r'\\n|;|\n',txt):
            line=line.replace('\\t',' ').strip()
            if not line: ins['<empty>']+=1; continue
            mn=line.split()[0]
            ins[mn]+=1; examples.setdefault(mn,(str(p),line))
for k,v in ins.most_common(): print(v,k,examples.get(k))
