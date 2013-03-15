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
import random
import unittest
class TestQuicksort(TimedTest):
    def test_qs(self):
        n = 100000
        #x = [random.random() for _ in xrange(100000)] 
        x = [random.randint(0,10000000) for _ in xrange(n)]
        self.time_compare(qs, x, repeat=5)
#        self.run_falcon(qs, x)

if __name__ == '__main__':
    unittest.main()
