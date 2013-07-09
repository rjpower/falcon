from testing_helpers import wrap 

@wrap
def modulo_const(a):
  return a % 22 

def test_modulo_const():
  modulo_const(10)
  modulo_const(322)
  modulo_const(390.49)
  modulo_const(-32)
  modulo_const(-2903.9)
  modulo_const("%d")
 

@wrap 
def modulo(a,b):
  return a % b 

def test_modulo():
  modulo(3,49)
  modulo(49,3)
  modulo(True, True)
  modulo(3.39, True)
  modulo("%d", 3)
  modulo("%d", 3.39)
  modulo("%s", 3.93)
  modulo("%s", "hello")
  modulo("%s %s", ("a", "b"))
  modulo("%s %s", (1,2))
  
