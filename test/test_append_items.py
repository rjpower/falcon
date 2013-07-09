from testing_helpers import wrap 

@wrap
def append_items(n):
  x = []
  for i in range(n):
    x.append(i)

def test_append_items():
  append_items(1000)



