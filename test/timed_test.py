import dis
import falcon
import logging
import sys
import time
import unittest 



logging.basicConfig(
    format='%(asctime)s %(filename)s:%(funcName)s %(message)s',
    level=logging.INFO, 
    stream=sys.stderr)

class TimedTest(unittest.TestCase):
  def time_compare(self, function, *args, **kw):
    print 'Original bytecode:\n%s\n' % dis.dis(function)
    repeat = kw.get('repeat', 1)
    evaluator = falcon.Evaluator()
    frame = evaluator.buildFrameFromPython(function, args)
    
    for i in range(repeat):
      st = time.time()
      py_result = function(*args)
      py_time = time.time() - st
      
      st = time.time()
      falcon_result = evaluator.eval(frame)
      f_time = time.time() - st
      
      logging.info('%s : Python: %.3f, Falcon: %.3f' % (function.func_name, py_time, f_time))
      self.assertEqual(py_result, falcon_result)
