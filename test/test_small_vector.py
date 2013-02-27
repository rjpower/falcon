#!/usr/bin/env python

import unittest
import falcon

class TestCase(unittest.TestCase):
  def test_small_vector_1(self):
    v = falcon.SmallIntVector()
    v.push_back(100)
    self.assertEqual(v.at(0), 100)
    
  def test_small_vector_2(self):
    v = falcon.SmallIntVector()
    for i in range(100):
      v.push_back(i)
    for i in range(100):
      self.assertEqual(v.at(i), i)

if __name__ == '__main__':
  unittest.main()
