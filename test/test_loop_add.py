import falcon

@falcon.wrap
def add_list_elts(l):
    su = 0
    for li in l:
        su += li
    return su

def test_add_list_elts():
  l = range(35)
  assert add_list_elts(l) == sum(l)
