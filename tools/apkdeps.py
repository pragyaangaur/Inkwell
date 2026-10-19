#!/usr/bin/env python3
"""Resolves Alpine packages plus their dependencies from APKINDEX files,
skipping what the minirootfs already has. Prints one repo/name-version per
line, where repo comes from the index's file name (APKINDEX.main gives main).
Several indexes go in one argument, separated by commas. With --exact it
prints only the packages named, without dependencies.
Usage: apkdeps.py [--exact] APKINDEX[,APKINDEX...] package..."""
import sys
args = sys.argv[1:]
exact = args[:1] == ['--exact']
if exact: args = args[1:]
idx, wanted = args[0], args[1:]
pkgs, provides = {}, {}
for path in idx.split(','):
    repo = path.rsplit('.', 1)[-1]
    for block in open(path).read().split('\n\n'):
        f = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ':':
                f.setdefault(line[0], line[2:])
        if 'P' not in f or f['P'] in pkgs: continue
        f['repo'] = repo
        pkgs[f['P']] = f
        provides[f['P']] = f['P']
        for p in f.get('p', '').split():
            provides.setdefault(p.split('=')[0], f['P'])
base = set('alpine-baselayout alpine-baselayout-data alpine-keys alpine-release apk-tools busybox busybox-binsh ca-certificates-bundle libapk libcrypto3 libssl3 musl musl-utils scanelf ssl_client zlib'.split())
seen, out = set(base), []
def add(name):
    name = name.split('=')[0].split('>')[0].split('<')[0].split('~')[0]
    if name.startswith('!'): return
    p = provides.get(name)
    if not p or p in seen: return
    seen.add(p)
    for d in pkgs[p].get('D', '').split(): add(d)
    out.append(pkgs[p]['repo'] + '/' + p + '-' + pkgs[p]['V'])
if exact:
    for w in wanted: out.append(pkgs[w]['repo'] + '/' + w + '-' + pkgs[w]['V'])
else:
    for w in wanted: add(w)
print('\n'.join(out))
