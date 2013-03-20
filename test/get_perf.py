#!/usr/bin/env python

import cPickle
import subprocess
import os, sys, math, re

tests = [
  'count_threshold',
  'crypto',
  'decision_tree',
  'fannkuch',
  'fasta',
  'matmult',
  'quicksort', 
  'wordcount',
  ]

print 'test,opt,op_count,stack_count,reg_count,python_time,falcon_time'
n_repeats = 5
for t in tests:
  for opt in range(2):
    py_opcount, opcount = 0, 0
    registers = 0
    results = []
    for rep in range(n_repeats):
      if opt == 0: prefix = 'DISABLE_OPT=1'
      else: prefix = ''
    
      stdin, stdout, stderr = os.popen3('bash -c "%s python test/test_%s.py"' % (prefix, t))
      stdin.close()
      out = stdout.read()
      err = stderr.read()
    
      lines = err.split('\n')
      for l in lines:
        match = re.search('.*PERFORMANCE (\w+) : Python ([0-9.]+), Falcon: ([0-9.]+)', l)
        if match:
          f, pytime, ftime = match.groups()
          pytime = float(pytime)
          ftime = float(ftime)
          results.append((pytime, ftime))
        if rep == 0:
          match = re.search('.*COMPILED (\w+), (\d+) registers, (\d+) operations, (\d+)', l)
          if match:
            f, reg, ops, py_ops = match.groups()
            py_opcount += int(py_ops)
            opcount += int(ops)
            registers += int(reg)
    
    for pytime, ftime in results:
      vals = [t, opt, opcount, py_opcount, registers, pytime, ftime]
      print ','.join([str(x) for x in vals])  
