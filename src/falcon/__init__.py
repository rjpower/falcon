from falcon_core import *
import falcon_core
import os
import sys

evaluator = Evaluator()

def run_function(f, *args, **kw):
  print "NO WRAPPER", "ARGS = ", args, "KW =", kw
  return evaluator.eval_python(f, args, kw)
  #frame = evaluator.frame_from_pyfunc(f, args, kw)
  #return evaluator.eval(frame)


def wrap(f):
  '''Function decorator.  
  
  Functions wrapped in this decorator will be compiled and run via falcon.
  '''
  def wrapper(*args, **kw):
    print "CALLING with args =", args, "kw =", kw 
    return evaluator.eval_python(f, args, kw)
    #frame = evaluator.frame_from_pyfunc(f, args, kw)
    #return evaluator.eval(frame)
  wrapper.func_name = f.func_name
  return wrapper

    
