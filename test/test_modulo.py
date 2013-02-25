#!/usr/bin/env python 

from timed_test import simple_test
import unittest

@simple_test
def test_modulo():
  return  '%d %d %s' % (100, 100, "Hello")
  
@simple_test
def test_modulo2():
  return 100 % 10

if __name__ == '__main__':
  unittest.main()
