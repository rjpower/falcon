from testing_helpers import wrap 

@wrap 
def compare(a, b):
  if a < b:
    return 10
  else:
    return -10
  
def test_compare_numbers():
  compare(1,0)
  compare(1,1)
  compare(1,2)
  compare(1,True)
  compare(1,False)
  compare(1.0,1)
  compare(1.0,1.0)
  compare(-1.0, -1.0)
  compare(-2.0, -3.0)
  compare(-2.0, False)
  
def test_compare_tuples():
  compare((1,1), (2,2))
  
def test_compare_strings():
  compare("hello", "hello")
  compare("hello", "hello2")
  compare("hello", "hell")