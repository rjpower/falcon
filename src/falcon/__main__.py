import os
import sys
import falcon
import imp 

# Wrap the main function of a script and run it inside of falcon. 
def main():
  if not sys.argv[1:]:
    print 'Usage: -mfalcon <script> <args>'
    sys.exit(2)

  script = sys.argv[1]
  sys.argv = sys.argv[1:]
  
  sys.path.insert(0, os.path.dirname(script))
  with open(script, 'rb') as fp:
      code = compile(fp.read(), script, 'exec')
  
  m = imp.new_module("__main__")
  d = m.__dict__ 
  d['__builtins__'] = __builtins__
  d['__file__'] = script 
  e = falcon.Evaluator()
  e.eval_python_module(code, d)
  
if __name__ == '__main__':
  main()
