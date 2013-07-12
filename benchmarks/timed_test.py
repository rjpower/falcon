import dis
import logging
import random
import sys
import time
import unittest
import numpy as np
logging.basicConfig(
    format='%(asctime)s %(filename)s:%(funcName)s %(message)s',
    level=logging.INFO,
    stream=sys.stderr)

def function_name(f):
  if sys.version_info.major >= 3: return f.__name__
  return f.func_name

try:
  import falcon
except:
  logging.warn('Failed to import falcon.', exc_info=1)
  falcon = None

class TimedTest(unittest.TestCase):
  def timed(self, function, *args, **kw):
    return self.time_compare(function, *args, **kw)

  def run_falcon(self, function, *args, **kw):
    evaluator = falcon.Evaluator()
    return evaluator.eval_python(function, args, None)


  def time_compare(self, function, *args, **kw):
#    print 'Original bytecode, %s:\n' % function.func_name
#    dis.dis(function)

    if 'repeat' in kw:
      repeat = kw['repeat']
      del kw['repeat']

    if falcon:
      evaluator = falcon.Evaluator()

    py_times = []
    f_times = []
    for i in range(repeat):
      random.seed(10)
      st = time.time()
      py_result = function(*args)
      py_times.append(time.time() - st)

      if falcon:
        random.seed(10)
        st = time.time()
        falcon_result = evaluator.eval_python(function, args, kw)
        f_times.append(time.time() - st)
      else:
        f_time = 0

      if falcon:
        if isinstance(py_result, list):
            # long lists seem to take a bizarrely long time for assertEqual
            # so I added a special case here
            assert isinstance(falcon_result, list), \
              "Expected list but got %s" % type(falcon_result)
            assert len(py_result) == len(falcon_result), \
              "Expected list of length %d but got list of %d" % \
              (len(py_result), len(falcon_result))
            for py_elt, falcon_elt in zip(py_result, falcon_result):
              assert py_elt == falcon_elt, "%s != %s" % (py_elt, falcon_elt)
        else:
            self.assertEqual(py_result, falcon_result)
    f_time_sum = np.sum(f_times)
    # f_time_std = np.std(f_times)
    py_time_sum = np.sum(py_times)
    # py_time_std = np.std(py_times)
    logging.info('PERFORMANCE %s (n = %d): Python %.3f, Falcon: %.3f' % \
            (function_name(function), repeat, py_time_sum, f_time_sum))

# decorator for quickly creating simple test functions
def simple_test(f):
  class TestCase(TimedTest):
    def test_function(self):
      self.time_compare(f)

  TestCase.__name__ = f.func_name
  return TestCase
