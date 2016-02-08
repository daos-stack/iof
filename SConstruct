import os, sys, subprocess

env = Environment()
config = Configure(env)
opts = Variables(["iof.conf"])

opts.Add(PathVariable('PREFIX', 'Alternate installation path',
                      os.path.join(Dir('#').abspath, 'install'),
                      PathVariable.PathIsDirCreate))
opts.Update(env)

env.Alias('install', env.get('PREFIX'))

ins_root = '$PREFIX'

Export('env opts ins_root config')

SConscript('SConscript', variant_dir='#build')

Help(opts.GenerateHelpText(env))

Import('all_deps')

all_deps.build()

env.Alias('deps', all_deps.targets)

SConscript('ping/SConscript', variant_dir="#build/ping")

Default('deps', 'ping')

opts.Save('iof.conf', env)
