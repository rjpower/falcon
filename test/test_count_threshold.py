def count(x,t):
  return sum([xi < t for xi in x])


from timed_test import TimedTest
import unittest 
import random
class TestCountThreshold(TimedTest):
    def test_qs(self):
        x = [random.random() for _ in xrange(10000000)] 
        self.time_compare(count, x, 0.5, repeat=1)

if __name__ == '__main__':
    unittest.main()
