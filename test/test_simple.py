#!/usr/bin/env python

import falcon
import logging
import opcode
import sys
import time
import unittest

logging.basicConfig(level=logging.INFO, stream=sys.stderr)

def add(a, b):
  return a + b

def compare(a, b):
  if a < b:
    return 10
  else:
    return -10

def loop(count):
  x = 0
  for i in xrange(count):
    x = x * 0
  return x

def infinite_loop():
  while 1: pass
  
def count_threshold(list, threshold):
  count = 0
  for item in list:
    if item > threshold: count += 1
  return count

class Simple(unittest.TestCase):
  def time_compare(self, function, *args):
    st = time.time()
    py_result = function(*args)
    py_time = time.time() - st
    
    st = time.time()
    falcon_result = falcon.run_function(function, *args)
    f_time = time.time() - st
    
    logging.info('%s : Python: %.3f, Falcon: %.3f' % (function.func_name, py_time, f_time))
    self.assertEqual(py_result, falcon_result)

  def test_add1(self): self.time_compare(add, 1, 2)
  def test_add2(self): self.time_compare(add, 100, 200)
  def test_add3(self): self.time_compare(add, 10 * 50, 2)
  
  def test_compare1(self): self.time_compare(compare, 10, 100)
    
  def test_loop1(self):
    self.time_compare(loop, 100)
    
  def test_loopbig(self):
    evaluator = falcon.Evaluator()
    evaluator.evalPython(loop, (1000 * 1000 * 10,))
    evaluator.dumpStatus()
    
  def test_count_threshold1(self):
    l = range(10)
    self.time_compare(count_threshold, l, 5) 
    
  def test_count_threshold2(self):
    l = range(10 * 1000 * 1000)
    self.time_compare(count_threshold, l, len(l) / 2) 
    
#  def test_infinite_loop(self):
#    falcon.run_function(infinite_loop)
#        
#  def test_nop_loop(self):
#    st = falcon.CompilerState()
#    bb = st.alloc_bb(0)
#    bb.code.push_back(falcon.CompilerOp(opcode.opmap['POP_BLOCK'], 0))
#    bb.code.push_back(falcon.CompilerOp(opcode.opmap['JUMP_ABSOLUTE'], 0))
#    code = falcon.compileRegCode(st)
#    evaluator = falcon.Evaluator()
#    frame = evaluator.buildFrameFromRegCode(code)
#    evaluator.eval(frame)
    
if __name__ == '__main__':
  unittest.main()
