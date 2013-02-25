#!/usr/bin/env python

from timed_test import simple_test
import unittest

class Foo(object):
  def __init__(self):
    self.a = None
    self.b = None

@simple_test
def store_attr():
  x = Foo()
  x.a = 0
  x.b = 10
  
@simple_test
def store_load_attr():
  x = Foo()
  x.a = 0
  x.b = 10
  return x.a + x.b

if __name__ == '__main__':
  unittest.main()