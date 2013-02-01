all:
	python setup.py build -f -g -q
	python setup.py develop

clean:
	rm -rf build/
	rm -f src/*.so

shell:
	bash
