import os
import sys
import falcon_core
from falcon_core import *

def run_function(f, *args):
  evaluator = Evaluator()
  frame = evaluator.buildFrameFromPython(f, args)
  return evaluator.eval(frame)