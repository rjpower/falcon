import math 

from  testing_helpers import wrap 

@wrap
def add_floors(count):
  total = 0
  for i in xrange(count):
    math.floor(i+0.5)
  return total 

def test_add_floors():
  add_floors(100)
  
@wrap
def add_ceils(count):
  total = 0
  for i in xrange(count):
    math.ceil(i+0.5)
  return total 

def test_add_ceils():
  return add_ceils(100)
