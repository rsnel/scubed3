# drawstat.py LOG...: self-draw rate of the juggler in daemon logs vs i.i.d. expectation
import sys, re, math
for fn in sys.argv[1:]:
    draws = selfs = 0; exp = 0.0; chain = longest = 0; old = False
    for line in open(fn, errors="replace"):
        m = re.search(r"drawn twice, writing it out empty \(pool=(\d+)\)", line)
        if m:
            n = int(m.group(1)); draws += 1; selfs += 1; exp += 1.0/n
            chain += 1; longest = max(longest, chain); continue
        m = re.search(r"new block \d+ seqno=\d+ next=\d+ pool=(\d+)", line)
        if m:
            n = int(m.group(1)); draws += 1; exp += 1.0/n; chain = 0; continue
        if "is its own tail" in line:
            old = True; selfs += 1; chain += 1; longest = max(longest, chain)
        elif "new block" in line: draws += 1; chain = 0
    if old:
        print("%-48s old juggler: %5d writes, %4d empty (%.1f%%), longest chain %d"
              % (fn[-48:], draws + selfs, selfs, 100.0*selfs/max(1, draws + selfs), longest))
    else:
        print("%-48s %5d draws, same block twice %4d, expected %6.1f (z = %+.2f), longest chain %d"
              % (fn[-48:], draws, selfs, exp, (selfs - exp)/math.sqrt(max(exp, 1e-9)), longest))
