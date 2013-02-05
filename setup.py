#!/usr/bin/python

import glob
import platform
import sys
import os

import ez_setup
ez_setup.use_setuptools()

from setuptools import setup, Extension
system, node, release, version, machine, processor = platform.uname()

setup(
  name='peregrine-falcon',
  version='0.01',
  maintainer='Russell Power',
  maintainer_email='russell.power@gmail.com',
  url='http://github.com/rjpower/peregrine-falcon',

  classifiers=[
    'Development Status :: 4 - Beta',
    'Environment :: Other Environment',
    'Intended Audience :: Developers',
    'License :: OSI Approved :: BSD License',
    'Operating System :: POSIX',
    'Programming Language :: C++',
    'Programming Language :: Python',
    'Programming Language :: Python :: 2.6',
    'Programming Language :: Python :: 2.7',
    'Programming Language :: Python :: 3.0',
    'Programming Language :: Python :: 3.1',
    'Programming Language :: Python :: 3.2',
    'Programming Language :: Python :: 3.3',
  ],

  description='Faster then a speeding bullet...',

  package_dir={'': 'src'},
  packages=['falcon'],

  ext_modules=[
    Extension('_falcon_core',
      include_dirs=['./src'],
      sources=[
              'src/falcon/reval.cc',
              'src/falcon/rcompile.cc',
              'src/falcon/util.cc',
              'src/falcon/rmodule.i'
              ],
      swig_opts = ['-Isrc', '-modern', '-O', '-c++',],
      extra_compile_args=['-O0', '-fno-gcse', '-fno-crossjumping', '-ggdb2', '-std=c++0x'],
      extra_link_args=['-lrt'],
    )
  ]
)
