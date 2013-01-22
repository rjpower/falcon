#!/usr/bin/env python
# encoding: utf-8
'''
Generates C++ code for executing registerized bytecode.
@copyright:  2013 Russell Power. All rights reserved.
@license:  Apache License 2.0
'''


import logging
import sys
import os
from opcode import opmap, opname

from optparse import OptionParser

class Ops:
  pass

for name, op in opmap.items():
  setattr(Ops, name, op) 

def fmt(s):
  f = sys._getframe()
  instance_vars = [(k, getattr(k)) for k in dir(f.f_back.f_locals['self'])]
  local_vars = f.f_back.f_locals
  all_vars = dict(local_vars)
  for k, v in instance_vars:
    if not k in all_vars:
      all_vars[k] = v
  
  return s % all_vars

def generator_to_string(g):
  if isinstance(g, str): return g
  return '\n'.join(g)

class OpcodeGenerator(object):
  def __init__(self, opcode, num_src_reg, num_dst_reg):
    self.opcode = opcode
    self.num_src_reg = num_src_reg
    self.num_dst_reg = num_dst_reg
    self.num_reg = num_dst_reg + num_src_reg
    
  def src(self, idx): return 'src_%d' % idx
  def dst(self, idx): return 'dst_%d' % idx
  
  def _load_register(self, idx, dst):
    return '%s = registers[op->reg_%d];' % (dst, idx)
  
  def _store_register(self, idx, value):
    return 'registers[op->reg_%d] = %s;' % (idx + self.num_src_reg, value)
   
  def generate(self):
    yield 'op_%s:' % opname[self.opcode]
    yield '{'
    yield 'RegisterOp%d op = *((RegisterOp%d*)(code + offset));' % (self.num_reg, self.num_reg)
    for r in range(self.num_src_reg): yield self._load_register(r, 'PyObject* %s' % self.src(r))
    yield generator_to_string(self._generate())
    for r in range(self.num_dst_reg): yield self._store_register(r, self.dst(r))
    yield '}'
    

class BinOpGenerator(OpcodeGenerator):
  def __init__(self, opcode, cfunc):
    OpcodeGenerator.__init__(self, opcode, num_src_reg=2, num_dst_reg=1)
    self.cfunc = cfunc
    
  def _generate(self):
    return '%s = %s(%s, %s);' % (self.dst(0), self.cfunc, self.src(0), self.src(1))

opcode_to_gen = {}
opcode_to_gen[Ops.BINARY_POWER] = BinOpGenerator(Ops.BINARY_POWER, 'PyNumber_Power')
opcode_to_gen[Ops.BINARY_MULTIPLY] = BinOpGenerator(Ops.BINARY_MULTIPLY, 'PyNumber_Multiply')
opcode_to_gen[Ops.BINARY_DIVIDE] = BinOpGenerator(Ops.BINARY_DIVIDE, 'PyNumber_Divide')
opcode_to_gen[Ops.BINARY_MODULO] = BinOpGenerator(Ops.BINARY_MODULO, 'PyNumber_Modulo') 
opcode_to_gen[Ops.BINARY_ADD] = BinOpGenerator(Ops.BINARY_ADD, 'PyNumber_Add')
opcode_to_gen[Ops.BINARY_SUBTRACT] = BinOpGenerator(Ops.BINARY_SUBTRACT, 'PyNumber_Subtract')
opcode_to_gen[Ops.BINARY_SUBSCR] = BinOpGenerator(Ops.BINARY_SUBSCR, 'PyNumber_Subscr')
opcode_to_gen[Ops.BINARY_FLOOR_DIVIDE] = BinOpGenerator(Ops.BINARY_FLOOR_DIVIDE, 'PyNumber_FloorDivide')
opcode_to_gen[Ops.BINARY_TRUE_DIVIDE] = BinOpGenerator(Ops.BINARY_TRUE_DIVIDE, 'PyNumber_TrueDivide')

def main(argv=None):
  program_name = os.path.basename(sys.argv[0])

  if argv is None:
    argv = sys.argv[1:]

  parser = OptionParser()
  parser.add_option("-o", "--out", dest="outfile", help="set output path [default: %default]", metavar="FILE")
  parser.add_option("-v", "--verbose", dest="verbose", action="count", help="set verbosity level [default: %default]")
  parser.set_defaults(outfile="./out.txt", infile="./in.txt")
  (opts, args) = parser.parse_args(argv)
  
  logging.basicConfig(format='%(asctime)s %(filename)s:%(funcName)s %(message)s',
                      level=logging.DEBUG if opts.verbose else logging.INFO)
  
  print 'void PyRegEval_EvalFrame(PyFrameObject* f) {'
  print 'static void* labels[] = {'
  for idx in range(len(opname)):
    if idx in opcode_to_gen:
      print '&&op_%s,' % opname[idx]
    else:
      print '&&op_BAD,'
  print '};'
  for opcode, generator in opcode_to_gen.items():
    print generator_to_string(generator.generate())
  print '}'

if __name__ == "__main__":
  sys.exit(main())
