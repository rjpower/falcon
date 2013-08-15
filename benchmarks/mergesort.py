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



import random 

if __name__ == '__main__':
  n = 10000000
  randint = random.randint 
  x = [randint(0,n/200) for _ in xrange(n)] * 200 
  mergesort(x)
  