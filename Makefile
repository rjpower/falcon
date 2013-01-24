export

PATH := $(shell readlink -f ./bin/):$(PATH)
PYTHONHOME := $(shell readlink -f .)

all:
	python setup.py build -f
	python setup.py develop

clean:
	rm -rf build/
	rm -f src/*.so

shell:
	bash
