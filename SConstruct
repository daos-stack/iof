# Copyright (C) 2016-2017 Intel Corporation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted for any purpose (including commercial purposes)
# provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions, and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions, and the following disclaimer in the
#    documentation and/or materials provided with the distribution.
#
# 3. In addition, redistributions of modified forms of the source or binary
#    code must carry prominent notices stating that the original code was
#    changed and the date of the change.
#
#  4. All publications or advertising materials mentioning features or use of
#     this software are asked, but not required, to acknowledge that it was
#     developed by Intel Corporation and credit the contributors.
#
# 5. Neither the name of Intel Corporation, nor the name of any Contributor
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""Build iof components"""
import os
import sys
sys.path.insert(0, os.path.join(Dir('#').abspath, "scons_local"))
try:
    from prereq_tools import PreReqComponent
except ImportError:
    raise ImportError \
          ("\'prereq_tools\' module not found; run \'git submodule update\'")

IOF_VERSION = "0.0.1"

# Desired compiler flags that will be used if the compiler supports them.
DESIRED_FLAGS = ['-Wdeclaration-after-statement',
                 '-Wno-missing-braces',
                 '-Wunknown-warning-option',
                 '-Wno-gnu-designator',
                 '-Wno-gnu-zero-variadic-macro-arguments']

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

def check_flag(context, flag):
    """Helper function to allow checking for compiler flags"""

    cc_name = context.env.get('CC')
    context.Message("Checking %s %s " % (cc_name, flag))
    context.env.Replace(CFLAGS=['-Werror', flag])
    ret = context.TryCompile("""
int main() {
    return 0;
}
""", ".c")
    context.Result(ret)
    return ret

def run_checks(env, platform, check_flags):
    """Run all configure time checks"""

    cenv = env.Clone()
    cenv.Append(CFLAGS='-Werror')
    config = Configure(cenv, custom_tests={'CheckFlag' : check_flag})
    try:
        cmd = 'setfattr'
        if platform == 'Darwin':
            cmd = 'xattr'

        if not config.CheckProg(cmd):
            print '%s command not installed, extended attribute test ' \
               'will not work' % cmd

    except AttributeError:
        print 'CheckProg not present'

    if config.CheckHeader('stdatomic.h'):
        env.AppendUnique(CPPDEFINES=['HAVE_STDATOMIC=1'])

    # Check for configure flags.
    # Some configure flags are always enabled (-g etc) however check in the
    # compiler supports other ones before using them.  Additional flags
    # might have been specified by the user in ~/.scons_localrc so check
    # those as well as DESIRED_FLAGS, and do it in such a way as user
    # flags come last so they can be used to disable locally defined flags.

    # Any flag that doesn't start with -W gets passed through unmodified.

    checked = []
    for flag in check_flags:
        if flag in checked:
            continue
        if not flag.startswith('-W') or config.CheckFlag(flag):
            env.Append(CFLAGS=[flag])
        checked.append(flag)
    config.Finish()

    print 'Compiler options: %s %s' % (env.get('CC'),
                                       ' '.join(env.get('CFLAGS')))


def scons():
    """Scons function"""
    platform = os.uname()[0]
    opts_file = os.path.join(Dir('#').abspath, 'iof-%s.conf' % platform)

    commits_file = os.path.join(Dir('#').abspath, 'build.config')
    if not os.path.exists(commits_file):
        commits_file = None

    env = DefaultEnvironment()
    arch_dir = 'build/%s' % platform
    VariantDir(arch_dir, '.', duplicate=0)

    if os.path.exists('iof.conf') and not os.path.exists(opts_file):
        print 'Renaming legacy conf file'
        os.rename('iof.conf', opts_file)

    opts = Variables(opts_file)
    prereqs = PreReqComponent(env, opts,
                              config_file=commits_file, arch=platform)

    prereqs.preload(os.path.join(Dir('#').abspath,
                                 "scons_local",
                                 "components.py"),
                    prebuild=["ompi", "cart"])

    Export('env prereqs IOF_VERSION')

    # Pull out the defined CFLAGS to use them later on for checking.
    check_flags = list(DESIRED_FLAGS)
    check_flags.extend(env.get('CFLAGS'))
    env.Replace(CFLAGS='')
    env.Append(CFLAGS=['-g', '-Wall', '-std=gnu99'])

    opts.Add(BoolVariable('fuse3',
                          'Use libfuse3 from github',
                          True))
    opts.Add(EnumVariable('client_libs',
                          'Build the client libraries', 'shared',
                          allowed_values=('none', 'shared', 'static', 'all'),
                          ignorecase=2))

    opts.Update(env)

    if not env.GetOption('clean'):
        run_checks(env, platform, check_flags)
    opts.Save(opts_file, env)

    unknown = opts.UnknownVariables()
    if unknown:
        print "Unknown variables: %s" % unknown.keys()
        SetOption("help", True)

    env.Alias('install', "$PREFIX")

    SConscript('%s/src/SConscript' % arch_dir)
    SConscript('%s/test/SConscript' % arch_dir)
    SConscript('%s/scons_local/test_runner/SConscript' % arch_dir)

    env.Install('$PREFIX/etc', ['utils/memcheck-iof.supp'])

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
