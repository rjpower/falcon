#!/usr/bin/env python

import falcon
import logging
import opcode
import sys
import time
import unittest
import math

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
  
def count_threshold(limit, threshold):
  count = 0
  for item in xrange(limit):
    if item > threshold: count += 1
  return count

def global_math(count):
  for i in xrange(count):
    math.floor(i)

def unpack_first(x):
    a,b,c = x
    return a

class Simple(unittest.TestCase):
  def time_compare(self, function, *args, **kw):
    repeat = kw.get('repeat', 1)
    evaluator = falcon.Evaluator()
    frame = evaluator.buildFrameFromPython(function, args)
    
    for i in range(repeat):
      st = time.time()
      py_result = function(*args)
      py_time = time.time() - st
      
      st = time.time()
      falcon_result = evaluator.eval(frame)
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
    self.time_compare(count_threshold, 10, 5) 
    
  def test_count_threshold2(self):
    self.time_compare(count_threshold, 1*1000*1000, 4*100*1000, repeat=15)
    
  def test_global_load(self): 
    self.time_compare(global_math, 1000000, repeat=15)
  
  def test_unpack_first(self):
    self.time_compare(unpack_first, (1,2,3), repeat = 1)
    
if __name__ == '__main__':
  unittest.main()
