import dis
import falcon
import sys

class wrap(object):
  def __init__(self, f):
    self.python_fn = f
    self.name = f.__name__ 
    self.falcon_fn = falcon.wrap(f)
    
  def __call__(self, *args, **kwargs):
    python_result = self.python_fn(*args, **kwargs)
    falcon_result = self.falcon_fn(*args, **kwargs)
    assert python_result == falcon_result, \
      "%s failed: expected %s but got  %s" % (self.name, python_result, falcon_result) 
