from falcon_core import *
import falcon_core
import os
import sys

def run_function(f, *args):
  evaluator = Evaluator()
  frame = evaluator.frame_from_python(f, args, None)
  return evaluator.eval(frame)
