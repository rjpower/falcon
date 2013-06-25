Falcon
======

CPython, made faster.

Falcon is an extension module for Python which implements a optimized, register machine based interpreter,
inside of your interpreter.  You specify which functions you want Falcon to wrap (or your entire module), and
Falcon takes over execution from there.  

Performance improvements vary from not at all (sorry) to up to 3 times faster (yay!).

Getting the code:

    git clone github.com/rjpower/falcon
    
    # optional, setup virtualenv
    virtualenv .
    source bin/activate

    python setup.py develop
    python test/test_count_threshold.py

Using Falcon:
    
    import falcon
    
    @falcon.wrap
    def my_slow_function()
        ...

_or_

    python -m falcon my_module.py

Try it out, and let us know what you think!


## Questions

**How is Falcon different from CPython**

Falcon "compiles" ordinary Python bytecode by converting from CPython's stack-based representation to a 
[register bytecode](http://stackoverflow.com/questions/11120343/advantages-of-stack-based-bytecodes-or-infinite-register-machines). 
Falcon then rewrites this bytecode by appluying some rudimentary dataflow optimizations and executes it using [direct-threaded dispatch](https://blog.mozilla.org/dmandelin/2008/06/03/squirrelfish/).
Falcon also stores integers directly in registers (without construct PyInt objects) using [bit tagging](http://mail.python.org/pipermail/python-dev/2004-July/046139.html), which 
can yield significant speed improvements on arithmetic-heavy code. 

**How is Falcon different from PyPy?**

PyPy is a tracing compiler, whereas Falcon is just an efficient interpreter implementation. PyPy might make 
The main motivation for Falcon is that an approach like PyPy breaks extension modules written using the C API, 
whereas Falcon is an attempt to see how fast you can get Python while maintaining a PyObject representation 
(which extension modules depend on). 


