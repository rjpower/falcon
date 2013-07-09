from testing_helpers import wrap 

@wrap 
def nested(x):
  def f(y):
    return y+y
  return f(x)

def test_nested():
  nested(3)
  nested(3.0)
  nested([1])
  
@wrap   
def nested_closure(x):
  def f(y):
    return x + y
  return f(x)

def test_nested_closure():
  nested_closure(3)
  nested_closure(3.0)
  nested_closure([1])
  

@wrap
def nested_closure_repeat():
  for i in xrange(50):
    temp = nested_closure(i)
  return temp 

def test_nested_closure_repeat():
  nested_closure_repeat()
  

if __name__ == '__main__':
  import nose 
  nose.main()