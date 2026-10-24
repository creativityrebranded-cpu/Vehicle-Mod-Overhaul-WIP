"""Check the actual command resolver, context/gesture collisions and device isolation.
Refuses a stale executable; this replaces the former textual binding-row check.
"""
import argparse
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=root/'plugin' if (root/'plugin/src').is_dir() else root
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--test',type=Path)
args=parser.parse_args()
candidates=[source/p/'Release/commandinput_test.exe' for p in ('build-binding-repair-20261024','build')]
exe=args.test or next((p for p in candidates if p.is_file()),None)
if not exe or not exe.is_file():sys.exit('Build commandinput_test first, or supply --test PATH.')
inputs=[source/'src'/p for p in ('bindlogic.h','bindingdefs.h','commandinput.h','inputownership.h')]
inputs.append(source/'tests/commandinput_test.cpp')
if any(p.stat().st_mtime>exe.stat().st_mtime for p in inputs):sys.exit('commandinput_test is older than its source; rebuild it first.')
sys.exit(subprocess.run([str(exe.resolve())]).returncode)
