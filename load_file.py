#!/usr/bin/python

# Parse a config file and return the PREFIX used by scons_local to
# install the code.

import os
import sys

def main(filename):
    values = {}
    if not os.path.exists(filename):
        return
    exec open(filename, 'rU').read() in {}, values
    prefix=values.get('PREFIX',os.path.realpath('install'))
    print 'PREFIX=%s' % prefix

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(0)
    main(sys.argv[1])
