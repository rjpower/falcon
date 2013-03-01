def qs(x):
  if len(x) <= 1:
      return x
  
  midpoint = len(x) / 2
  pivot = x[midpoint]
  eq = []
  less = []
  greater = []
  for xi in x:
    if xi < pivot:
        less.append(xi)
    elif xi > pivot:
        greater.append(xi)
    else:
        eq.append(xi)
  return qs(less) + eq + qs(greater)



from timed_test import TimedTest
import unittest 
import random
class TestQuicksort(TimedTest):
    def test_qs(self):
        x = [random.random() for _ in xrange(100000)] 
        self.time_compare(qs, x, repeat=1)

if __name__ == '__main__':
    unittest.main()
