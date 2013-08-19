
from testing_helpers import wrap 

@wrap
def construct_and_get_fields(a,b):
    class A(object):
        def __init__(self, a):
            self.a = a
    class B(A):
        def __init__(self, a, b):
            A.__init__(self, a)
            self.b = b 
    obj = A(a)
    obj2 = B(a,b)
    return obj.a, obj2.a, obj2.b 

def test_nested_objects():
    (a1, a2, a3) = construct_and_get_fields(1,2.0)
    assert a1 == 1
    assert a2 == 1
    assert a3 == 2.0


if __name__ == '__main__':
  test_nested_objects()