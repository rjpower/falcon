import random
import timed_test 
import unittest 

class Node(object):
    def __init__(self, idx, thresh, left, right):
      self.idx = idx
      self.thresh = thresh
      self.left = left
      self.right = right

    def predict(self, x):
      if x[self.idx] < self.thresh:
          return self.left.predict(x)
      else:
          return self.right.predict(x)

    def __eq__(self, other):
        return other.__class__ is Node and \
               self.idx == other.idx and \
               self.thresh == other.thresh and \
               self.left == other.left and \
               self.right == other.right

class Leaf(object):
    def __init__(self, value):
        self.value = value
    
    def predict(self, x):
        return self.value 

    def __eq__(self, other):
        return other.__class__ is Leaf and \
               other.value == self.value


def gen_random_tree(n_features, depth):
  #print "n_features = %d, depth = %d" % (n_features, depth)
  if depth == 0:
      return Leaf(random.random())
  else:
    feature_idx = random.randint(0, n_features-1)
    threshold = random.random()
    left = gen_random_tree(n_features, depth - 1)
    right = gen_random_tree(n_features, depth - 1)
    return Node(feature_idx, threshold, left, right)


def predict_labels(feature_list, tree):
  return [tree.predict(v) for v in feature_list]

def gen_random_tuples(n_items, n_features):
  return [tuple([random.random() for _ in xrange(n_features)]) 
          for _ in xrange(n_items)]

class TestDecisionTree(timed_test.TimedTest):
  def test_gen_features(self):
    self.timed(gen_random_tuples, 1000,100)
  def test_gen_tree(self):
    self.timed(gen_random_tree, 1000, 12)
  def test_eval_labels(self):
    tree = gen_random_tree(n_features = 1000, depth = 12)
    features = gen_random_tuples(5000,1000)
    self.timed(predict_labels, features, tree)

if __name__ == '__main__':
  unittest.main()


    


