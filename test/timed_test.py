import dis
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
    try: import falcon
    except: falcon = None

    print 'Original bytecode:\n'
    dis.dis(function)
    
    repeat = kw.get('repeat', 1)
    if falcon:
      evaluator = falcon.Evaluator()
      frame = evaluator.frame_from_python(function, args)
    
    for i in range(repeat):
      st = time.time()
      py_result = function(*args)
      py_time = time.time() - st
     
      if falcon:
        st = time.time()
        falcon_result = evaluator.eval(frame)
        f_time = time.time() - st
      else:
        f_time = 0
      
      logging.info('%s : Python: %.3f, Falcon: %.3f' % (function.func_name, py_time, f_time))
      if falcon:
        self.assertEqual(py_result, falcon_result)

