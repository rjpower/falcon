
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


if __name__ == '__main__':
  
  n = 200 
  x = make_matrix(n,n)
  z = make_matrix(n,n)
  mm_loops(x, x, z)

  