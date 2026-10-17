#!/usr/bin/env python3
"""Resolves Alpine packages plus their dependencies from an APKINDEX, skipping
what the minirootfs already has. Prints one name-version per line. With
--exact it prints only the packages named, without dependencies.
Usage: apkdeps.py [--exact] APKINDEX package..."""
import sys
args = sys.argv[1:]
exact = args[:1] == ['--exact']
if exact: args = args[1:]
idx, wanted = args[0], args[1:]
pkgs, provides = {}, {}
for block in open(idx).read().split('\n\n'):
    f = {}
    for line in block.splitlines():
        if len(line) > 2 and line[1] == ':':
            f.setdefault(line[0], line[2:])
    if 'P' not in f: continue
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
    out.append(p + '-' + pkgs[p]['V'])
if exact:
    for w in wanted: out.append(w + '-' + pkgs[w]['V'])
else:
    for w in wanted: add(w)
print('\n'.join(out))
