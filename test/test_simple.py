#!/usr/bin/env python

import tempest
import unittest

def add(a, b):
  return a + b

def loop(count):
  for i in range(count):
    pass
  return count

class TestSimpleFunctions(unittest.TestCase):
  def test_add1(self):
    self.assertEqual(tempest.run_function(add, (1,2)), 3)
  
  def test_add2(self):
    self.assertEqual(tempest.run_function(add, (100,200)), 300)
    
  def test_add3(self):
    self.assertEqual(tempest.run_function(add, (10**50,200)), 10**50+200)
    
  def test_loop1(self):
    self.assertEqual(tempest.run_function(loop, (100,)), 100)
    
if __name__ == '__main__':
  unittest.main()
