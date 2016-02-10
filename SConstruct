import os, sys, subprocess

opts_file = os.path.join(Dir('#').abspath, "iof.conf")
opts = Variables([opts_file])
env = Environment(variables=opts)
config = Configure(env)

Export('opts env config')

SConscript('SConscript', variant_dir='#build')

Import('all_deps ins_root')

Help(opts.GenerateHelpText(env))

opts.Save(opts_file, env)

unknown = opts.UnknownVariables()
if unknown:
    print "Unknown variables: %s"%unknown.keys()
    SetOption("help", True)

all_deps.build()

env.Alias('deps', all_deps.targets)

SConscript('ping/SConscript', variant_dir="#build/ping")

Default('deps', 'ping')
