#!/usr/bin/env python

import unittest
from timed_test import simple_test

@simple_test
def store_slice():
  x = range(100)
  x[10:20] = range(50, 60)
  return x

@simple_test
def store_slice1():
  x = range(100)
  x[10:] = range(50, 60)
  return x

@simple_test
def store_slice2():
  x = range(100)
  x[:10] = range(50, 60)
  return x

@simple_test
def store_slice3():
  x = range(100)
  x[:] = range(50, 60)
  return x


@simple_test
def load_slice0():
  x = range(100)
  y = x[10:20]
  return y

@simple_test
def load_slice1():
  x = range(100)
  y = x[10:]
  return y

@simple_test
def load_slice2():
  x = range(100)
  y = x[:10]
  return y

@simple_test
def load_slice3():
  x = range(100)
  y = x[:]
  return y

@simple_test
def load_slice4():
  x = range(100)
  y = x[1::-1]
  return y

if __name__ == '__main__':
  unittest.main()
