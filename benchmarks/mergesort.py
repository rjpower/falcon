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




if __name__ == '__main__':
  x = [10,9,8,7,6,5,4,3,2,1] * 20000 
  mergesort(x)
  
