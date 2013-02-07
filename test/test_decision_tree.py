import random
import timed_test 
import unittest 

def gen_random_tree(n_features, depth):
    if depth == 0:
      return random.random()
    else:
      feature_idx = random.randint(0, n_features)
      threshold = random.random()
      left = gen_random_tree(depth - 1)
      right = gen_random_tree(depth - 1)
      return (feature_idx, threshold, left, right)


def eval_feature_tuple(features, tree):
  if isinstance(tree, tuple):
    if features[tree[0]] < tree[1]:
      return eval_feature_tuple(features, tree[2])
    else:
      return eval_feature_tuple(features, tree[3])



