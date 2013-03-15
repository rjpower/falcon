import bisect 
import sys
import unittest 

from timed_test import TimedTest

alu = (
   'GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGG'
   'GAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGA'
   'CCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAAT'
   'ACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCA'
   'GCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGG'
   'AGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCC'
   'AGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA')

iub = zip('acgtBDHKMNRSVWY', [0.27, 0.12, 0.12, 0.27] + [0.02] * 11)

homosapiens = [
    ('a', 0.3029549426680),
    ('c', 0.1979883004921),
    ('g', 0.1975473066391),
    ('t', 0.3015094502008),
]


def genRandom(lim, ia=3877, ic=29573, im=139968):
    seed = 42
    imf = float(im)
    while 1:
        seed = (seed * ia + ic) % im
        yield lim * seed / imf

Random = genRandom(1.)

def makeCumulative(table):
    P = []
    C = []
    prob = 0.
    for char, p in table:
        prob += p
        P += [prob]
        C += [char]
    return (P, C)

def repeatFasta(src, n):
    width = 60
    r = len(src)
    s = src + src + src[:n % r]
    for j in xrange(n // width):
        i = j * width % r
        #print s[i:i + width]
    if n % width:
        pass
        #print s[-(n % width):]

def randomFasta(table, n):
    width = 60
    r = xrange(width)
    gR = Random.next
    bb = bisect.bisect
    jn = ''.join
    probs, chars = makeCumulative(table)
    for j in xrange(n // width):
      pass
        #print jn([chars[bb(probs, gR())] for i in r])
    if n % width:
      pass
        #print jn([chars[bb(probs, gR())] for i in xrange(n % width)])

class TestFasta(TimedTest):
  def test_three_homosapiens(self):
    self.time_compare(randomFasta, homosapiens, 150000000, repeat = 5)
    
if __name__ == '__main__':
  unittest.main() 
