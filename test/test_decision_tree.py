import random
import timed_test 
import unittest 

def gen_random_tree(n_features, depth):
  if depth == 0:
    return random.random()
  else:
    feature_idx = random.randint(0, n_features)
    threshold = random.random()
    left = gen_random_tree(n_features, depth - 1)
    right = gen_random_tree(n_features, depth - 1)
    return (feature_idx, threshold, left, right)


def find_label(features, tree):
  if isinstance(tree, tuple):
    if features[tree[0]] < tree[1]:
      return find_label(features, tree[2])
    else:
      return find_label(features, tree[3])
  else:
    return tree

def find_labels(feature_list, tree):
  return [find_label(v, tree) for v in feature_list]

def gen_random_tuples(n_items, n_features):
  return [tuple([random.random() for _ in xrange(n_features)]) 
          for _ in xrange(n_items)]

class TestDecisionTree(timed_test.TimedTest):
  def test_gen_features(self):
    self.timed(gen_random_tuples, 1000,100)
  def test_gen_tree(self):
    self.timed(gen_random_tree, 100, 5)
  def test_eval_labels(self):
    tree = gen_random_tree(100,5)
    features = gen_random_tuples(1000,100)
    self.timed(find_labels, features, tree)

if __name__ == '__main__':
  unittest.main()


    


