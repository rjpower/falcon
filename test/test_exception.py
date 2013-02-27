#!/usr/bin/env python
import unittest

from timed_test import TimedTest
import falcon

def a(x): return b(x)
def b(x): return c(x)
def c(x): return throws(x)

def throws(x):
  x[100] = 0

class TestExceptionHandling(TimedTest):
  def test_simple(self):
    f_eval = falcon.Evaluator()
    frame = f_eval.frame_from_pyfunc(a, (0,), None)
    f_eval.eval(frame)

        
    
if __name__ == '__main__':
  unittest.main()