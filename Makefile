# To allow for parallel and incremental builds (since I'm an impatient person),
# this Makefile duplicates some of the logic of setup.py, rather then just 
# shelling out to it.  Any additions to the setup.py extension should also
# be made here!

ifndef REALBUILD

opt: 
	mkdir -p build/opt
	cd build/opt && REALBUILD=1 $(MAKE) -f ../../Makefile opt
	ln -sf ../build/opt/_falcon_core.so src/_falcon_core.so

dbg: 
	mkdir -p build/dbg
	cd build/dbg && REALBUILD=1 $(MAKE) -f ../../Makefile dbg 
	ln -sf ../build/dbg/_falcon_core.so src/_falcon_core.so

clean:
	rm -rf build/
else
TOPDIR := ../..
SRCDIR := $(TOPDIR)/src

VPATH := $(SRCDIR)/falcon
CPPFLAGS := -I$(SRCDIR) -I$(TOPDIR)/include/python2.7 -I$(SRCDIR)/sparsehash-2.0.2/src
# -fno-gcse -fno-crossjumping 

CFLAGS := $(CPPFLAGS) -Wall -pthread -fno-strict-aliasing -fwrapv -Wall -fPIC -ggdb2 -std=c++0x -funroll-loops
CXXFLAGS := $(CFLAGS) 
INCLUDES := $(shell find $(SRCDIR) -name '*.h') ../../Makefile

opt : COPT := -O3 -funroll-loops
dbg : COPT := -DFALCON_DEBUG=1 -O0 -fno-omit-frame-pointer

opt: _falcon_core.so
dbg: _falcon_core.so

%.o : %.cc $(INCLUDES) 
	$(CXX) $(COPT) $(CXXFLAGS) -c $< -o $@

%.o : %.cpp  $(INCLUDES) 
	$(CXX) $(COPT) $(CXXFLAGS) -c $< -o $@

_falcon_core.so: rlist.o reval.o rcompile.o rinst.o rmodule_wrap.o util.o oputil.o rexcept.o
	 g++ -shared -o $@ $^ -lrt

$(SRCDIR)/falcon/rmodule_wrap.cpp: $(SRCDIR)/falcon/rmodule.i $(INCLUDES) 
	swig -python -Isrc -modern -O -c++ -w312,509 -o $(SRCDIR)/falcon/rmodule_wrap.cpp $(SRCDIR)/falcon/rmodule.i

clean:
	rm -rf build/
	rm -f src/*.so

endif
