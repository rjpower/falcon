opt:
	CFLAGS='-O3 -fno-omit-frame-pointer' python setup.py build -f -g -q
	python setup.py develop

dbg:
	CFLAGS='-O0 -DFALCON_DEBUG=1' python setup.py build -f -g -q
	python setup.py develop

clean:
	rm -rf build/
	rm -f src/*.so

shell:
	bash
