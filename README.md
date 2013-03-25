Falcon
======

CPython, made faster.

Falcon is an extension module for Python which implements a optimized, register machine based interpreter,
inside of your interpreter.  You specify which functions you want Falcon to wrap (or you entire module), and
Falcon takes over execution from there.  

Performance improvments vary from not at all (sorry) to up to 3 times faster (yay!).

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
