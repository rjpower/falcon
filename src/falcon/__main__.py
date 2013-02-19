import os
import sys
import falcon


# Wrap the main function of a script and run it inside of falcon. 
def main():
    if not sys.argv[1:]:
      print 'Usage: -mfalcon <script> <args>'
      sys.exit(2)

    print sys.argv
    script = sys.argv[1]
    sys.argv = sys.argv[1:]

    sys.path.insert(0, os.path.dirname(script))
    with open(script, 'rb') as fp:
        code = compile(fp.read(), script, 'exec')
    globs = {
        '__file__': script,
        '__name__': '__main__',
        '__package__': None,
    }
    
    f_eval = falcon.Evaluator()
    try:
      frame = f_eval.frame_from_codeobj(code)
    except:
      print 'Failed to compile code.  Running normally.'
      eval(code, globs, {})
    else:
      f_eval.eval(frame)
  
if __name__ == '__main__':
  main()
