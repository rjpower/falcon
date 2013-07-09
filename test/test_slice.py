from testing_helpers import wrap 

@wrap
def store_slice():
  x = range(100)
  x[10:20] = range(50, 60)
  return x

def test_store_slice():
  store_slice()

@wrap
def store_slice1():
  x = range(100)
  x[10:] = range(50, 60)
  return x

def test_store_slice1():
  store_slice1()

@wrap
def store_slice2():
  x = range(100)
  x[:10] = range(50, 60)
  return x

def test_store_slice2():
  store_slice2()

@wrap
def store_slice3():
  x = range(100)
  x[:] = range(50, 60)
  return x


def test_store_slice3():
  store_slice3()


@wrap
def load_slice0():
  x = range(100)
  y = x[10:20]
  return y


def test_load_slice0():
  load_slice0()

@wrap
def load_slice1():
  x = range(100)
  y = x[10:]
  return y


def test_load_slice1():
  load_slice1()

@wrap
def load_slice2():
  x = range(100)
  y = x[:10]
  return y

def test_load_slice2():
  load_slice2()


@wrap
def load_slice3():
  x = range(100)
  y = x[:]
  return y

def test_load_slice3():
  load_slice3()

@wrap
def load_slice4():
  x = range(100)
  y = x[1::-1]
  return y

def test_load_slice4():
  load_slice4()

