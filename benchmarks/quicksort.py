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


if __name__ == '__main__':
  import argparse 
  import random 
  randint = random.randint 
  parser = argparse.ArgumentParser(description="Quicksort benchamrk")
  parser.add_argument('--length', type=int, default = 100000)
  args = parser.parse_args()
  n = args.length
  x = x = [randint(0, n) for _ in xrange(n)]
  qs(x)  
