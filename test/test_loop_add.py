from  testing_helpers import test 

@test
def add_list_elts(l):
    su = 0
    for li in l:
        su += li
    return su


def test_add_list_elts():
  l = range(35)
  add_list_elts(range(35))
  