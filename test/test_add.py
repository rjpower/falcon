#!/usr/bin/env python

import unittest
from timed_test import simple_test

@simple_test
def add():
  x = []
  for i in range(1000):
    x.append(i)

if __name__ == '__main__':
  unittest.main()
