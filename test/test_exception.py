from testing_helpers import wrap 

def a(x): return b(x)
def b(x): return c(x)
def c(x): return throws(x)

 
def throws(x):
  x[100] = 0

@wrap
def capture(x):
  try:
    a(x)
    return 0
  except:
    return 1

def test_exceptions():
  capture( (0,) )
