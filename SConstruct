"""Build iof components"""
import os
import sys
sys.path.insert(0, os.path.join(Dir('#').abspath, "scons_local"))
from prereq_tools import PreReqComponent

def scons():
    """Scons function"""

    platform = os.uname()[0]
    opts_file = os.path.join(Dir('#').abspath, 'iof-%s.conf' % platform)
    env = DefaultEnvironment()
    arch_dir = 'build/%s' % platform
    VariantDir(arch_dir, '.', duplicate=0)

    if os.path.exists("SConscript.local"):
        # pylint: disable=pointless-string-statement
        """Define this file in order to modify environment defaults
        #For example:
        Import('env')
        import os
        OS_ENV = ENV['ENV']
        OS_ENV["PATH"] = "/foo/bar" + os.pathsep + OS_ENV["PATH"]
        env.Replace(ENV=OS_ENV)"""
        SConscript('SConscript.local', 'env')
        # pylint: enable=pointless-string-statement

    if os.path.exists('iof.conf') and not os.path.exists(opts_file):
        print 'Renaming legacy conf file'
        os.rename('iof.conf', opts_file)

    opts = Variables(opts_file)
    prereqs = PreReqComponent(env, opts, arch=platform)
    prereqs.preload(os.path.join(Dir('#').abspath,
                                 "scons_local",
                                 "components.py"),
                    prebuild=["ompi", "mercury", "mcl"])

    Export('env prereqs')

    env.Append(CFLAGS=['-g', '-Wall', '-Wdeclaration-after-statement',
                       '-std=gnu99', '-pedantic', '-Wno-missing-braces',
                       '-Wno-gnu-zero-variadic-macro-arguments'])

    opts.Add(BoolVariable('fuse3',
                          'Use libfuse3 from github',
                          False))

    opts.Update(env)
    try:
        config = Configure(env)
        cmd = 'setfattr'
        if platform == 'Darwin':
            cmd = 'xattr'

        if not config.CheckProg(cmd):
            print '%s command not installed, extended attribute test ' \
               'will not work' % cmd

    except AttributeError:
        print 'CheckProg not present'

    opts.Save(opts_file, env)

    unknown = opts.UnknownVariables()
    if unknown:
        print "Unknown variables: %s" % unknown.keys()
        SetOption("help", True)

    env.Alias('install', "$PREFIX")

    SConscript('%s/src/SConscript' % arch_dir)
    Default('src')

    # Pick up any directories under 'proto' which have a SConscript file
    for fname in os.listdir('proto'):
        if not os.path.exists('proto/%s/SConscript' % fname):
            continue
        SConscript('%s/proto/%s/SConscript' % (arch_dir, fname))
        Default('proto/%s' % fname)

    # Put this after all SConscript calls so that any imports they require can
    # be included.
    build_info = prereqs.get_build_info()
    build_info.gen_script(".build_vars-%s.sh" % platform)
    build_info.save(".build_vars-%s.py" % platform)

    try:
        #if using SCons 2.4+, provide a more complete help
        Help(opts.GenerateHelpText(env), append=True)
    except TypeError:
        Help(opts.GenerateHelpText(env))

if __name__ == "SCons.Script":
    scons()
