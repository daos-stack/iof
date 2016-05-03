"""Build iof components"""
import os
import sys
sys.path.insert(0, os.path.join(Dir('#').abspath, "scons_local"))
from prereq_tools import PreReqComponent
PLATFORM = os.uname()[0]
OPTS_FILE = os.path.join(Dir('#').abspath, 'iof-%s.conf' % PLATFORM)
ENV = DefaultEnvironment()
ARCH_DIR = 'build/%s' % PLATFORM
VariantDir(ARCH_DIR, '.', duplicate=0)

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

if os.path.exists('iof.conf') and not os.path.exists(OPTS_FILE):
    print 'Renaming legacy conf file'
    os.rename('iof.conf', OPTS_FILE)

OPTS = Variables(OPTS_FILE)
PREREQS = PreReqComponent(ENV, OPTS, arch=PLATFORM)
PREREQS.preload(os.path.join(Dir('#').abspath,
                             "scons_local",
                             "components.py"),
                prebuild=["ompi", "mercury", "mcl"])

Export('ENV PREREQS')

ENV.Append(CFLAGS=['-g', '-Wall', '-Wdeclaration-after-statement', '-std=gnu99',
                   '-pedantic', '-Wno-missing-braces'])

OPTS.Add(BoolVariable('fuse3',
                      'Use libfuse3 from github',
                      False))

OPTS.Update(ENV)

OPTS.Save(OPTS_FILE, ENV)

UNKNOWN = OPTS.UnknownVariables()
if UNKNOWN:
    print "Unknown variables: %s" % UNKNOWN.keys()
    SetOption("help", True)

ENV.Alias('install', "$PREFIX")

SConscript('%s/src/SConscript' % ARCH_DIR)
Default('src')

# Pick up any directories under 'proto' which have a SConscript file
for fname in os.listdir('proto'):
    if not os.path.exists('proto/%s/SConscript' % fname):
        continue
    SConscript('%s/proto/%s/SConscript' % (ARCH_DIR, fname))
    Default('proto/%s' % fname)

# Put this after all SConscript calls so that any imports they require can be
# included.
BUILD_INFO = PREREQS.get_build_info()
BUILD_INFO.gen_script(".build_vars-%s.sh" % PLATFORM)
BUILD_INFO.save(".build_vars-%s.py" % PLATFORM)

try:
    #if using SCons 2.4+, provide a more complete help
    Help(OPTS.GenerateHelpText(ENV), append=True)
except TypeError:
    Help(OPTS.GenerateHelpText(ENV))
