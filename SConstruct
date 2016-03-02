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
    SConscript('SConscript.local')
# pylint: enable=pointless-string-statement

OPTS = Variables(OPTS_FILE)
PREREQS = PreReqComponent(ENV, OPTS)
PREREQS.preload(os.path.join(Dir('#').abspath,
                             "scons_local",
                             "components.py"),
                prebuild=["ompi", "mercury"])

Export('ENV PREREQS')

Help(OPTS.GenerateHelpText(ENV))

OPTS.Save(OPTS_FILE, ENV)

UNKNOWN = OPTS.UnknownVariables()
if UNKNOWN:
    print "Unknown variables: %s" % UNKNOWN.keys()
    SetOption("help", True)

SConscript('ping/SConscript', variant_dir="#build/ping")

Default('ping')
