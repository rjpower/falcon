import os
import sys
import ctypes
from ctypes import py_object

_libnames = ['_tempest.so', '_tempest_d.so']
_libloc = None
_lib = None

for d in sys.path:
  for n in _libnames:
    if os.path.exists(os.path.join(d, n)):
      _libloc = os.path.join(d, n)
      break

if not _libloc:
  raise Exception, 'Failed to find native library in system path!'

_lib = ctypes.cdll.LoadLibrary(_libloc)
eval_frame = _lib.RegisterEvalFrame
eval_frame.restype = py_object
eval_frame.argtypes = [py_object]

compile_codeobj = _lib.RegisterCompileCode
compile_codeobj.restype = py_object
compile_codeobj.argtypes = [py_object]

run_function = _lib.RegisterRunFunction
run_function.restype = py_object
run_function.argtypes = [py_object, py_object]