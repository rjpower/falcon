def fannkuch(n):
    maxFlipsCount = 0
    permSign = True
    checksum = 0

    perm1 = list(range(n))
    count = perm1[:]
    rxrange = range(2, n - 1)
    nm = n - 1
    while 1:
        k = perm1[0]
#        print k
        if k:
            perm = perm1[:]
            flipsCount = 1
            kk = perm[k]
            while kk:
#                print 'while kk:: ', perm, k, kk
                perm[:k+1] = perm[k::-1]
                flipsCount += 1
                k = kk
                kk = perm[kk]
            if maxFlipsCount < flipsCount:
                maxFlipsCount = flipsCount
            checksum += flipsCount if permSign else -flipsCount

        # Use incremental change to generate another permutation
#        print 'Permsign', permSign
        if permSign:
            perm1[0],perm1[1] = perm1[1],perm1[0]
            permSign = False
        else:
            perm1[1],perm1[2] = perm1[2],perm1[1]
            permSign = True
            for r in rxrange:
                if count[r]:
                    break
                count[r] = r
                perm0 = perm1[0]
                perm1[:r+1] = perm1[1:r+2]
                perm1[r+1] = perm0
            else:
#                print 'Count', count
                r = nm
                if not count[r]:
                    print( checksum )
                    return checksum
            count[r] -= 1

from timed_test import TimedTest
import unittest 

class TestF(TimedTest):
    def test_f(self):
      self.time_compare(fannkuch, 9, repeat=1)
      
if __name__ == '__main__':
  if len(sys.argv) > 2:
    n = int(sys.argv[2])
  else:
    n = 
  fannkuch(9 
  unittest.main() 
