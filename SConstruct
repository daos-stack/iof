"""Build iof components"""
import os
import sys
sys.path.insert(0, os.path.join(Dir('#').abspath, "scons_local"))
try:
    from prereq_tools import PreReqComponent
except ImportError:
    raise ImportError \
          ("\'prereq_tools\' module not found; run \'git submodule update\'")

def save_build_info(env, prereqs, platform):
    """Save the build information"""

    build_info = prereqs.get_build_info()

    #Save the build info locally
    json_build_vars = '.build_vars-%s.json' % platform
    sh_build_vars = '.build_vars-%s.sh' % platform
    build_info.gen_script(sh_build_vars)
    build_info.save(json_build_vars)

    #Install the build info to the testing directory
    env.InstallAs('$PREFIX/TESTING/.build_vars.sh',
                  sh_build_vars)
    env.InstallAs('$PREFIX/TESTING/.build_vars.json',
                  json_build_vars)

def scons():
    """Scons function"""

    platform = os.uname()[0]
    opts_file = os.path.join(Dir('#').abspath, 'iof-%s.conf' % platform)
    env = DefaultEnvironment()
    arch_dir = 'build/%s' % platform
    VariantDir(arch_dir, '.', duplicate=0)

    if os.path.exists('iof.conf') and not os.path.exists(opts_file):
        print 'Renaming legacy conf file'
        os.rename('iof.conf', opts_file)

    opts = Variables(opts_file)
    prereqs = PreReqComponent(env, opts, arch=platform)

    if os.path.exists("SConscript.local"):
        SConscript('SConscript.local', 'env')

    prereqs.preload(os.path.join(Dir('#').abspath,
                                 "scons_local",
                                 "components.py"),
                    prebuild=["ompi", "mercury", "mcl"])

    Export('env prereqs')

    env.Append(CFLAGS=['-g', '-Wall', '-Wdeclaration-after-statement',
                       '-std=gnu99', '-pedantic', '-Wno-missing-braces'])

    opts.Add(BoolVariable('fuse3',
                          'Use libfuse3 from github',
                          False))

    opts.Update(env)
    config = Configure(env)
    try:
        cmd = 'setfattr'
        if platform == 'Darwin':
            cmd = 'xattr'

        if not config.CheckProg(cmd):
            print '%s command not installed, extended attribute test ' \
               'will not work' % cmd

    except AttributeError:
        print 'CheckProg not present'
    config.Finish()

    opts.Save(opts_file, env)

    unknown = opts.UnknownVariables()
    if unknown:
        print "Unknown variables: %s" % unknown.keys()
        SetOption("help", True)

    env.Alias('install', "$PREFIX")

    SConscript('%s/src/SConscript' % arch_dir)
    SConscript('%s/test/SConscript' % arch_dir)

    # Pick up any directories under 'proto' which have a SConscript file
    for fname in os.listdir('proto'):
        if not os.path.exists('proto/%s/SConscript' % fname):
            continue
        SConscript('%s/proto/%s/SConscript' % (arch_dir, fname))
        Default('proto/%s' % fname)

    # Put this after all SConscript calls so that any imports they require can
    # be included.
    save_build_info(env, prereqs, platform)

    try:
        #if using SCons 2.4+, provide a more complete help
        Help(opts.GenerateHelpText(env), append=True)
    except TypeError:
        Help(opts.GenerateHelpText(env))

if __name__ == "SCons.Script":
    scons()
