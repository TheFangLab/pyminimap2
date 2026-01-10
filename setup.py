from setuptools import setup, Extension, find_packages
import os
import glob
import sys

# Define source files directory
minimap2_dir = 'minimap2'

# Collect all .c files in minimap2/
sources = glob.glob(os.path.join(minimap2_dir, '*.c'))

# Filter sources:
# Exclude example.c as it contains its own main()
sources = [s for s in sources if 'example.c' not in s]

# Add our custom wrapper
# The new wrapper is at src/pyminimap2/_pyminimap2.c
wrapper_source = os.path.join('src', 'pyminimap2', '_pyminimap2.c')
sources.append(wrapper_source)

# Define compiler flags and macros
# We remap main to mm2_main and exit to mm2_exit
define_macros = [
    ('main', 'mm2_main'),
    ('exit', 'mm2_exit'),
    ('HAVE_KALLOC', None),
    ('_FILE_OFFSET_BITS', '64'),
    ('_LARGEFILE64_SOURCE', None)
]

extra_compile_args = ['-g', '-Wall', '-O2']
libraries = ['m', 'z']

if sys.platform.startswith('linux'):
    extra_compile_args.extend(['-pthread', '-msse4.1'])
    libraries.append('pthread')

setup(
    name='pyminimap2',
    version='2.30.3',
    description='Python wrapper for minimap2 with stdout/stderr capture',
    # We use find_packages where='src' to locate the pyminimap2 package
    packages=find_packages(where='src'),
    package_dir={'': 'src'},
    ext_modules=[
        Extension(
            name='pyminimap2._core',
            sources=sources,
            include_dirs=[minimap2_dir],
            define_macros=define_macros,
            extra_compile_args=extra_compile_args,
            libraries=libraries
        )
    ]
)
