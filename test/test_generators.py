from testing_helpers import wrap 


@wrap 
def count_threshold_generator(limit, threshold):
  return sum(item > threshold for item in xrange(limit))

#def test_count_threshold_generator():
#  count_threshold_generator(1000,490)