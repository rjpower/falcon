def qs(x):
  if len(x) <= 1:
      return x
  midpoint = len(x) / 2
  pivot = x[midpoint]
  less = [xi for xi in x if xi < pivot]
  greater = [xi for xi in x if xi > pivot] 
  return qs(less) + [pivot] +  qs(greater)


from timed_test import TimedTest
import unittest 

class TestQuicksort(TimedTest):
    def test_qs(self):
        x = [1,2] * 1000000
        self.time_compare(qs, x, repeat=1)

if __name__ == '__main__':
    unittest.main()
