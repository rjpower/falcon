from testing_helpers import wrap 


def add_(a, b):
  return a + b

@wrap 
def add(a,b):
  return add_(a,b)
 
def test_add():
  add(1,1)
  add(-1,1)
  add(1.39,True)
  add(False,True)
  add([1],[2])
  add((1),(2))
  add("hello"," world")

@wrap 
def add1(a):
  return add_(a, 1)

def test_add1():
  add1(1)
  add1(1.0)
  add1(False)

@wrap 
def inplace_add(x):
  x[0] = 0
  x[0] += 10
  return x[0] 

def test_inplace_add():
  a = [0]
  inplace_add(a) 
  