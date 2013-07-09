from testing_helpers import wrap 

@wrap 
def count_threshold(limit, threshold):
  count = 0
  for item in xrange(limit):
    if item > threshold: count += 1
  return count

def test_count_threshold():
  count_threshold(1000, 50)
  
  