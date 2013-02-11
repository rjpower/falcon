import os
import sys
import falcon_core
from falcon_core import *

def run_function(f, *args):
  evaluator = Evaluator()
  frame = evaluator.frame_from_python(f, args, None)
  return evaluator.eval(frame)