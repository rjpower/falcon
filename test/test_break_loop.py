from testing_helpers import wrap 

@wrap
def loop_with_break(break_idx, total):
  for i in xrange(total):
    if i > break_idx:
      break
  return i 

def test_loop_with_break():
  loop_with_break(50000,100000)
  