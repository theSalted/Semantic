#!/usr/bin/env python3
"""Copy the measured source and apply only the 4B model-identity patch.

Fail closed if source has changed since measurement. Never edit production source.
Model files and build dependencies must already be installed; no downloads.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--model', type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
manifest = json.loads((here / 'source-manifest.json').read_text())

def digest(path):
    with path.open('rb') as stream:
        sha = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
        return sha.hexdigest()

if args.model.name != 'Qwen3-4B-Q4_K_M.gguf' or digest(args.model) != '7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5':
    parser.error('Expected the official, verified Qwen3-4B-Q4_K_M.gguf; see README.')
for name, sha in manifest.items():
    if Path(name).is_absolute() or '..' in Path(name).parts:
        parser.error('Invalid source manifest path')
    if digest(args.root / name) != sha:
        parser.error(f'Measured source changed: {name}. Review and revalidate the trial before updating the manifest.')
args.output.mkdir(parents=True, exist_ok=True)
target = Path(tempfile.mkdtemp(prefix='4b-', dir=args.output.resolve()))
for name in manifest:
    dest = target / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.root / name, dest)
subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(here / '4b-model.patch')],
               cwd=target, check=True, stdout=sys.stderr)
print(target)
