"""Lot switch gaps from ApexRadiance_LOG.txt (developer mode): after each active lot switch (RtOutdoorLamps "Active lot
tracker X"), how long until the new lot's relit maps are drawn ("Active: outdoor lamps ..." or "Every story ..."). Meanwhile
the new lot shows the game's own maps and the lot just left lost its relit ones (DropAll): each switch is seen as a light
change on both lots. A switch that another switch follows first never got its relit maps.

Usage: python lot_switch_gap.py <ApexRadiance_LOG.txt> [...]
"""
import re
import sys
def sec(t):
    h,m,x=t.split(':');return int(h)*3600+int(m)*60+float(x)
for path in sys.argv[1:]:
    sw=None; gaps=[]; drops=0
    for line in open(path,encoding='utf-8',errors='replace'):
        m=re.match(r'(\d\d:\d\d:\d\d\.\d+) ',line)
        if not m: continue
        t=sec(m.group(1))
        if 'relit maps dropped' in line: drops+=1
        if re.search(r'\[RtOutdoorLamps\] Active lot tracker [0-9A-F]+(?!:)( \(|$)',line.rstrip()):
            if sw is not None: gaps.append(None)
            sw=t
        elif sw is not None and ('[RtOutdoorLamps] Active: outdoor lamps' in line or '[RtOutdoorLamps] Every story' in line):
            gaps.append(t-sw); sw=None
    done=[g for g in gaps if g is not None]
    done.sort()
    print(path.split('/')[-2], 'switches',len(gaps),'drops',drops,'reached relit',len(done),'never (next switch first)',gaps.count(None),
          'median %.1f s'%done[len(done)//2] if done else '', 'max %.1f s'%done[-1] if done else '')
