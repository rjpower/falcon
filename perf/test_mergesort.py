def merge(left, right):
  result = []
  i ,j = 0, 0
  while i < len(left) and j < len(right):
    if left[i] <= right[j]:
      result.append(left[i])
      i += 1
    else:
      result.append(right[j])
      j += 1
  result += left[i:]
  result += right[j:]
  return result

def mergesort(x):
  if len(x) <= 1:
      return x
  middle = int(len(x) / 2)
  left = mergesort(x[:middle])
  right = mergesort(x[middle:])
  return merge(left, right)



from timed_test import TimedTest
import random
import unittest
class TestQuicksort(TimedTest):
    def test_qs(self):
        n = 100000
        #x = [random.random() for _ in xrange(100000)] 
        x = [random.randint(0,10000000) for _ in xrange(n)]
        self.time_compare(mergesort, x, repeat=20)
#        self.run_falcon(qs, x)

if __name__ == '__main__':
    unittest.main()
