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
  name='tempest',
  version='0.01',
  maintainer='Russell Power',
  maintainer_email='russell.power@gmail.com',
  url='http://code.google.com/p/py-leveldb/',

  classifiers=[
    'Development Status :: 4 - Beta',
    'Environment :: Other Environment',
    'Intended Audience :: Developers',
    'License :: OSI Approved :: BSD License',
    'Operating System :: POSIX',
    'Programming Language :: C++',
    'Programming Language :: Python',
    'Programming Language :: Python :: 2.4',
    'Programming Language :: Python :: 2.5',
    'Programming Language :: Python :: 2.6',
    'Programming Language :: Python :: 2.7',
    'Programming Language :: Python :: 3.0',
    'Programming Language :: Python :: 3.1',
    'Programming Language :: Python :: 3.2',
    'Programming Language :: Python :: 3.3',
  ],

  description='Python bindings for leveldb database library',

  packages=['tempest'],
  package_dir={'tempest': 'src/tempest'},

  ext_modules=[
    Extension('_tempest',
      include_dirs=['./src/C'],
      sources=[
              'src/c/reval.cc',
              'src/c/rcompile.cc',
      ],
      libraries=['stdc++'],
    )
  ]
)
