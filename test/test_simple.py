#!/usr/bin/env python

import tempest
import time
import unittest

def add(a, b):
  return a + b

def loop(count):
  x = 0
  for i in xrange(count):
    x = x * 0
  return x

def time_compare(function, args):
  st = time.time()
  function(*args)
  py_time = time.time() - st
  
  st = time.time()
  tempest.run_function(function, args)
  t_time = time.time() - st
  
  print 'Python: %.3f, Tempest: %.3f' % (py_time, t_time)

class TestSimpleFunctions(unittest.TestCase):
  def test_add1(self):
    self.assertEqual(tempest.run_function(add, (1,2)), 3)
  
  def test_add2(self):
    self.assertEqual(tempest.run_function(add, (100,200)), 300)
    
  def test_add3(self):
    self.assertEqual(tempest.run_function(add, (10**50,200)), 10**50+200)
    
  def test_loop1(self):
    self.assertEqual(tempest.run_function(loop, (100,)), loop(100))
    
  def test_loopbig(self):
    #time_compare(loop, (1000*1000*100,))
    tempest.run_function(loop, (1000*1000*100,))
    
if __name__ == '__main__':
  unittest.main()
