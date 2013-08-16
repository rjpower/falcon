import collections 
import glob 
import subprocess 
import time 
import numpy as np 

interps = [
  ("Python", ["python"]), 
  ("PyPy", ["pypy"]), 
  ("Falcon", ["python", "-m", "falcon"])
]

def run(n_repeats=3):
  
  timings = {}
  failed = {}
  for interp_name, _ in interps:
    timings[interp_name] = collections.defaultdict(list)
    failed[interp_name] = set([])
  
  names = set([])
  for i in xrange(n_repeats):
    print "Running benchmarks (repeat = %d / %d)" % (i+1, n_repeats)
    print "======================================"
    for filename in glob.glob('*.py'):
      if filename != __file__:
        names.add(filename)
        for (interp_name, interp_command) in interps:
          if filename not in failed[interp_name]:
            print "----"
            print "Running %s under %s" % (filename, interp_name)
            start_t = time.time() 
            cmd = interp_command + [filename]
            result = subprocess.call(cmd)
        
            if result == 0:
              t = time.time() - start_t

              print "Elapsed time: %0.4f" % t 
              timings[interp_name][filename].append(t)
            else:
              print "FAILED"
              failed[interp_name].add(filename)
      print 
      print 
    
  print "%-20s| %-20s" % ("Benchmark", "time") 
  for filename in names:
    print " " * 19, "|"
    print "%-20s| %-20s" % (filename, "") 
    for interp_name, _ in interps:
      if filename in failed[interp_name]:
        print "  %-18s|   %-16s" % (interp_name, "FAILED") 
        continue 
      times = timings[interp_name][filename]
      print "  %-18s| %10.4f (min = %0.4f, max = %0.4f)" % \
        (interp_name, np.median(times), np.min(times), np.max(times)) 

import sys

if __name__ == '__main__':
  if len(sys.argv) > 1:
    N = int(sys.argv[1])
  else:
    N = 3
  run(N) 
