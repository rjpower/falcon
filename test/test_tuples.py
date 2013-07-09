
from testing_helpers import wrap 


@wrap 
def unpack_first(x):
    a,b,c = x
    return a
  
def test_unpack_first():
  unpack_first((1,2,3))
  unpack_first([1,2,3])


@wrap 
def add_tuples(x,y):
  a = (x,x)
  b = (y,y)
  return a + b

def test_add_tuples():
  add_tuples(20,309.0)