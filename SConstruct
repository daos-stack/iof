"""Build iof components"""
import os
import sys
sys.path.insert(0, os.path.join(Dir('#').abspath, "scons_local"))
from prereq_tools import PreReqComponent
OPTS_FILE = os.path.join(Dir('#').abspath, "iof.conf")
ENV = DefaultEnvironment()

if os.path.exists("SConscript.local"):
# pylint: disable=pointless-string-statement
    """Define this file in order to modify environment defaults
#For example:
Import('ENV')
import os
OS_ENV = ENV['ENV']
OS_ENV["PATH"] = "/foo/bar" + os.pathsep + OS_ENV["PATH"]
ENV.Replace(ENV=OS_ENV)"""
    SConscript('SConscript.local', 'ENV')
# pylint: enable=pointless-string-statement

OPTS = Variables(OPTS_FILE)
PREREQS = PreReqComponent(ENV, OPTS)
PREREQS.preload(os.path.join(Dir('#').abspath,
                             "scons_local",
                             "components.py"),
                prebuild=["ompi", "mercury"])

Export('ENV PREREQS')

ENV.Append(CFLAGS=['-g', '-Wall', '-Wdeclaration-after-statement', '-std=c99',
                   '-pedantic'])

Help(OPTS.GenerateHelpText(ENV))

OPTS.Save(OPTS_FILE, ENV)

UNKNOWN = OPTS.UnknownVariables()
if UNKNOWN:
    print "Unknown variables: %s" % UNKNOWN.keys()
    SetOption("help", True)

SConscript('src/SConscript', variant_dir='#build/iof/src')
Default('src')

# Pick up any directories under 'proto' which have a SConscript file
for fname in os.listdir('proto'):
    if not os.path.exists('proto/%s/SConscript' % fname):
        continue
    SConscript('proto/%s/SConscript' % fname,
               variant_dir='#build/iof/proto/%s' % fname)
    Default('proto/%s' % fname)
