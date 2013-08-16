from testing_helpers import wrap
import unittest

def throws(x):
  x[100] = 0

def a(x): return b(x)
def b(x): return c(x)
def c(x): return throws(x)

@wrap
def test_capture(x):
  try:
    a(x)
    return 0
  except:
    return 1

@wrap
def test_simple_throw():
  try:
    a = 0
    raise a
  except:
    return 1
 
if __name__ == '__main__':
  #test_simple_throw()
  test_capture(100)
