#!/usr/bin/env python

import dis
import opcode
import sys
import time
import unittest
import math

from timed_test import TimedTest

def loop_with_break():
  for i in xrange(100000):
    if i > 50000:
      break

class TestBreakLoop(TimedTest):
  def test_simple(self):
    self.time_compare(loop_with_break)
    
if __name__ == '__main__':
  unittest.main()
