#!/usr/bin/env python3
"""Downloads everything the WSL build needs, on the Windows side.

Called by build-windows.bat. WSL networking can be much slower than Windows
on the same connection, so the source code, the PS1 compilers, the Python
packages and any missing Ubuntu packages are fetched here into a cache
folder; the WSL build reads it from the Windows drive (PE_DOWNLOADS in
scripts/build.sh).

  python scripts/fetch-windows.py <cache folder>

Run scripts/build-wsl.sh prepare <cache folder> in WSL first: it writes
wsl-python.txt and apt-uris.txt there. Needs git and Python 3.8+ (with pip).
Everything already in the cache is kept, so running it again only fetches
what changed. It downloads no game data.
"""
import hashlib
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
LINUX_PLATFORMS = ["manylinux2014_x86_64", "manylinux_2_17_x86_64",
                   "manylinux_2_28_x86_64", "manylinux_2_34_x86_64", "linux_x86_64"]


def retry(fn, *args):
    # Large downloads can drop; try three times.
    for n in (1, 2, 3):
        try:
            return fn(*args)
        except Exception as e:
            if n == 3:
                raise
            print(f"(attempt {n} failed: {e}; retrying)", file=sys.stderr)
            time.sleep(5)


def git(*args):
    # HTTP/1.1: long HTTP/2 transfers from GitHub can break off ("curl 92").
    r = subprocess.run(["git", "-c", "http.version=HTTP/1.1", *args],
                       check=True, text=True, stdout=subprocess.PIPE)
    return r.stdout.strip()


def has_commit(repo, commit):
    return subprocess.run(["git", "-C", str(repo), "cat-file", "-e", commit + "^{commit}"],
                          capture_output=True).returncode == 0


def is_shallow(repo):
    return git("-C", str(repo), "rev-parse", "--is-shallow-repository") == "true"


def git_cache(cache, name, url, commit, urls, full=False):
    """Bare repo <cache>/git/<name>.git holding only <commit>, no history.
    full: with all history instead (pip clones with --filter=blob:none, which
    loops forever against a shallow repository)."""
    repo = cache / "git" / f"{name}.git"
    if has_commit(repo, commit) and full and is_shallow(repo):
        # Git object files are read-only, which Windows will not delete.
        for p in repo.rglob("*"):
            p.chmod(0o777)
        shutil.rmtree(repo)
    if not has_commit(repo, commit):
        print(f"  {name} {commit[:12]}")
        git("init", "-q", "--bare", str(repo))
        if full:
            retry(git, "-C", str(repo), "fetch", "-q", url, "+refs/heads/*:refs/heads/*")
        else:
            retry(git, "-C", str(repo), "fetch", "-q", "--depth", "1", url, commit)
        git("-C", str(repo), "update-ref", "refs/heads/pin", commit)
        git("-C", str(repo), "symbolic-ref", "HEAD", "refs/heads/pin")
    # The build fetches this exact commit by its id, pip with a filter. Set
    # here: git ignores GIT_CONFIG_* for the serving side of a local fetch.
    git("-C", str(repo), "config", "uploadpack.allowAnySHA1InWant", "true")
    git("-C", str(repo), "config", "uploadpack.allowFilter", "true")
    urls.append((url, name))
    return repo


def show(repo, path):
    return git("-C", str(repo), "show", f"pin:{path}")


def gitlink(repo, path):
    return git("-C", str(repo), "ls-tree", "pin", path).split()[2]


def submodule_url(repo, path):
    for section in re.split(r"^\[submodule ", show(repo, ".gitmodules"), flags=re.M)[1:]:
        if re.search(rf"^\s*path\s*=\s*{re.escape(path)}\s*$", section, re.M):
            return re.search(r"^\s*url\s*=\s*(\S+)", section, re.M).group(1)
    sys.exit(f"error: no submodule {path} in {repo}")


def find(pattern, text, what):
    m = re.search(pattern, text, re.M)
    if not m:
        sys.exit(f"error: could not find {what}")
    return m.groups() if len(m.groups()) > 1 else m.group(1)


def download(url, dest, hashes=None):
    """Downloads url to dest unless dest is already there with the right hash."""
    def ok():
        if not dest.is_file():
            return False
        for algo, value in (hashes or {}).items():
            if hashlib.new(algo, dest.read_bytes()).hexdigest() != value:
                return False
        return True

    if ok():
        return
    print(f"  {dest.name}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")

    def fetch():
        with urllib.request.urlopen(url, timeout=60) as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        part.replace(dest)
        if not ok():
            dest.unlink()
            raise RuntimeError(f"{dest.name}: checksum mismatch")

    retry(fetch)


def apt_debs(cache):
    """The .deb files apt listed with: apt-get install --print-uris."""
    uris = cache / "apt-uris.txt"
    lines = uris.read_text().splitlines() if uris.is_file() else []
    algos = {"MD5Sum": "md5", "SHA1": "sha1", "SHA256": "sha256", "SHA512": "sha512"}
    for line in lines:
        m = re.match(r"'([^']+)' (\S+) \d+ (\w+):(\w+)", line)
        if not m:
            continue
        url, name, algo, value = m.groups()
        hashes = {algos[algo]: value} if algo in algos else None
        download(url, cache / "apt" / name, hashes)


def pip_packages(cache):
    """Linux wheels (or source packages) for the WSL Python, pinned in
    scripts/pip-lock.txt."""
    pyver = (cache / "wsl-python.txt").read_text().strip()
    lock = HERE / "scripts" / "pip-lock.txt"
    stamp = cache / "pip" / ".done"
    want = hashlib.sha256(lock.read_bytes() + pyver.encode()).hexdigest()
    if stamp.is_file() and stamp.read_text() == want:
        return
    print(f"  Python packages for Python {pyver}")
    cmd = [sys.executable, "-m", "pip", "download", "-q", "--disable-pip-version-check",
           "-d", str(cache / "pip"), "--no-deps", "--prefer-binary",
           "--python-version", pyver, "--implementation", "cp",
           "--abi", "cp" + pyver.replace(".", ""), "--abi", "abi3", "--abi", "none",
           "-r", str(lock)]
    for p in LINUX_PLATFORMS:
        cmd += ["--platform", p]
    retry(lambda: subprocess.run(cmd, check=True))
    stamp.write_text(want)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[2])
    cache = Path(sys.argv[1]).resolve()
    cache.mkdir(parents=True, exist_ok=True)

    print("Ubuntu packages")
    apt_debs(cache)

    print("Source code")
    urls = []
    setup_tree = (HERE / "scripts" / "setup-tree.sh").read_text()
    decomp = git_cache(cache, "decomp", find(r"^DECOMP_URL=(\S+)", setup_tree, "DECOMP_URL"),
                       find(r"^DECOMP_COMMIT=(\w+)", setup_tree, "DECOMP_COMMIT"), urls)
    psyz = git_cache(cache, "psyz", submodule_url(decomp, "third_party/psyz"),
                     gitlink(decomp, "third_party/psyz"), urls)
    git_cache(cache, "SDL", submodule_url(psyz, "external/SDL"),
              gitlink(psyz, "external/SDL"), urls)
    maspsx = show(decomp, "tools/scripts/setup_maspsx.sh")
    git_cache(cache, "maspsx", find(r"git clone \S* *(https://\S+)", maspsx, "maspsx URL"),
              find(r'^REV="(\w+)"', maspsx, "maspsx REV"), urls)
    url, commit = find(r"git\+(https://[^@\s]+)@(\w+)", show(decomp, "requirements.txt"),
                       "asm-differ in requirements.txt")
    git_cache(cache, "asm-differ", url, commit, urls, full=True)
    # Read by scripts/build.sh: "<url> <name>", one per line, LF endings.
    with open(cache / "git-urls.txt", "w", newline="\n") as f:
        f.writelines(f"{u} {n}\n" for u, n in urls)

    print("PS1 compilers")
    for script in ("setup_stock_cc1.sh", "setup_stock_cc281.sh"):
        text = show(decomp, f"tools/scripts/{script}")
        rel = find(r'^REL="(\S+)"', text, f"REL in {script}")
        asset = find(r'^\s*Linux\)\s*ASSET="(\S+)"', text, f"Linux ASSET in {script}")
        download(f"{rel}/{asset}", cache / "files" / asset)

    pip_packages(cache)
    print(f"Downloads ready in {cache}")


if __name__ == "__main__":
    main()
