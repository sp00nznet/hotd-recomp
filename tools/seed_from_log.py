#!/usr/bin/env python3
"""Add every unresolved dispatch target in run logs to work/run_seeds.json.

    py -3 tools/seed_from_log.py work/r1.log [more.log ...]
    py -3 run_lift.py --all

A function only an indirect call reaches can be missed by the catalog; the
run names it ("ICALL: unresolved VA 0x...") and run_lift.py lifts it next time.
"""
import json
import os
import re
import sys

SEEDS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work', 'run_seeds.json')

seeds = set(json.load(open(SEEDS))) if os.path.exists(SEEDS) else set()
before = len(seeds)
for log in sys.argv[1:]:
    text = open(log, encoding='utf-8', errors='replace').read()
    seeds |= {int(v, 16) for v in re.findall(r'unresolved VA 0x([0-9A-Fa-f]{8})', text)}
json.dump(sorted(seeds), open(SEEDS, 'w'), indent=0)
print('%d seeds (%d new) in %s' % (len(seeds), len(seeds) - before, SEEDS))
