import os, sys, re, subprocess
from enum import Enum

env = Environment()

opts = Variables("iof.conf")
opts.Add(PathVariable('PREFIX', 'Alternate installation path',
                      os.path.join(Dir('#').abspath, 'install'),
                      PathVariable.PathIsDirCreate))
opts.Update(env)

env.Alias('install', env.get('PREFIX'))

ins_root = '$PREFIX'

Export('env opts ins_root')

SConscript('SConscript', variant_dir='#build')

Help(opts.GenerateHelpText(env))

Import('all_deps')

all_deps.build()

env.Alias('deps', all_deps.targets)

Default('deps')

opts.Save('iof.conf', env)
