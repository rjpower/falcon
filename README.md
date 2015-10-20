Falcon
======

CPython, made faster.

Falcon is an extension module for Python which implements an optimized, register machine based interpreter,
inside of your interpreter.  You specify which functions you want Falcon to wrap (or your entire module), and
Falcon takes over execution from there.  

Performance improvements vary from not at all (sorry) to up to 3 times faster (yay!).

### Getting the code:

    git clone https://github.com/rjpower/falcon
    cd falcon

    # optional, setup virtualenv
    virtualenv .
    source bin/activate

    python setup.py develop
    python test/test_math.py

### Using Falcon:
    
    import falcon
    
    @falcon.wrap
    def my_slow_function()
        ...

_or_

    python -m falcon my_module.py

Try it out, and let us know what you think!


### Questions

**What is Falcon and how is it different from CPython?**

The usual Python implementation (called CPython) compiles Python syntax to a stack bytecode. 
Falcon translates CPython's stack-based representation to a 
[register-based virtual machine](http://stackoverflow.com/questions/11120343/advantages-of-stack-based-bytecodes-or-infinite-register-machines). 
Falcon then rewrites this bytecode by applying some rudimentary dataflow optimizations and executes it using [direct-threaded dispatch](https://blog.mozilla.org/dmandelin/2008/06/03/squirrelfish/).
Falcon also stores integers directly in registers (without constructing PyInt objects) using [bit tagging](http://mail.python.org/pipermail/python-dev/2004-July/046139.html), which 
can yield significant speed improvements on arithmetic-heavy code. 

**How is Falcon different from PyPy?**

PyPy is a tracing compiler, whereas Falcon is just an efficient interpreter implementation. 
PyPy might speed up your code by several orders of magnitude but it will also choke on any extension code
which depends on the Python C API. Falcon, on the other hand, aims only for modest performance gains but preserves
the PyObject data representation necessary to avoid breaking extension modules. 

**Does Falcon support all of Python?** 

Not yet! Lots of constructs (like catching exceptions, constructing objects, etc...) aren't implemented in the Falcon virtual machine.
However, this doesn't mean that programs which use these constructs won't run. Any missing functionality is routed through the Python C API, 
foregoing any potential performance benefit you might have gotten from Falcon. So, though Falcon isn't a complete Python implementation, 
it should still run all of your code. If you try Falcon and it crashes on some program then you've encountered a bug and should let us know. 
