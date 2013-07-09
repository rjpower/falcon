from testing_helpers import wrap

class Foo(object):
  def __init__(self):
    self.a = None
    self.b = None
  
  def __eq__(self, other):
    return self.a == other.a and self.b == other.b 

@wrap
def store_attr():
  x = Foo()
  x.a = 0
  x.b = 10
  return Foo 


def test_store_attr():
  store_attr()

@wrap
def store_load_attr(a,b):
  x = Foo()
  x.a = a
  x.b = b
  return x.a + x.b

def test_store_load_attr():
  store_load_attr(0, 10)

