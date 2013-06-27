import unittest
from timed_test import TimedTest
def mm_loops(X,Y,Z):
    m = len(X)
    n = len(Y)
    for i in xrange(len(X)):
        xi = X[i]
        for j in xrange(len(Y)):
            yj = Y[j]
            total = 0
            for k in xrange(len(yj)):
                total += xi[k] * yj[k]
            Z[i][j] = total
    return Z

def make_matrix(m,n):
    mat = []
    for i in xrange(m):
        mat.append(range(n))
    return mat 

class TestMatMult(TimedTest):
    def test_matmult(self):
        x = make_matrix(200,200)
        y = make_matrix(200,200)
        z = make_matrix(200,200)
        self.time_compare(mm_loops, x, y, z, repeat=1)

if __name__ == '__main__':
   unittest.main() 
