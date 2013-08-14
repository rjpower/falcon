
from falcon import wrap 
from random import randint

@wrap 
def call_randint(start, stop):
  return randint(start, stop)

def test_randint():
  for _ in xrange(100):
    x = call_randint(0,10)
    assert x >= 0
    assert x <= 10  
    
if __name__ == '__main__':
  test_randint()