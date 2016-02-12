"""
This script can be invoked by dependent components to build
various dependencies of IO forwarding and dependent projects.
None of these dependencies use scons directly so this all gets
executed as a pre-build step.   Currently, it checks the
default location for sources which is $top_dir/../<name>.  If
it doesn't find any sources there, it checks gets the sources
using the specified retriever.   It automatically detects install
target and adds them to the installation path specified in the
top level SConstruct and it automatically adds the library path
and include paths for the libraries to the default environment.

Eventually, it would be good to do this in a more SCons friendly
way but this is a start that can help developers using SCons in
dependent projects to get IOF and its dependencies built.
"""

import sys, os, hashlib, subprocess, socket, tarfile
Import('opts env config')

def updateValue(env, key, value):
   env[key] = value
   opts.args[key] = value

opts.Add(PathVariable('PREFIX', 'Alternate installation path',
                      os.path.join(Dir('#').abspath, 'install'),
                      PathVariable.PathIsDirCreate))
opts.Add(PathVariable("PREBUILT_PREFIX",
                      'Directory for prebuilt components',
                      None,
                      PathVariable.PathIsDir))
opts.Add(PathVariable("SRC_PREFIX",
                      'Directory to find sources for components',
                      None,
                      PathVariable.PathIsDir))
opts.Add(PathVariable("TARGET_PREFIX",
                      'Eventual installation path prefix for components',
                      None,
                      PathVariable.PathAccept))
opts.Update(env)

class component_group:
    def __init__(self):
        self.components = []
        self.targets = []
        self.libs={}

    def add(self, component):
        self.components.append(component)
        self.libs[component.subdir] = component.libs

    def build(self):
        if GetOption('help'):
            return
        if not prebuilt_prefix:
            for c in self.components:
                c.get()
        for c in self.components:
            c.build()
            self.targets = self.targets + c.targets
    def get_libs(self, name):
        return self.libs.get(name, [])

all_deps = component_group()

#checkout, if necessary, and build iof dependencies
real_env = {"PATH":"/bin:/usr/bin:/usr/local/bin",
            "HOME":os.environ["HOME"]}

auth_sock = os.environ.get("SSH_AUTH_SOCK")
if auth_sock:
    real_env["SSH_AUTH_SOCK"] = auth_sock

try:
    socket.gethostbyname("proxy-chain.intel.com")
    real_env["http_proxy"] = "http://proxy-chain.intel.com:911"
    real_env["https_proxy"] = "https://proxy-chain.intel.com:912"
except:
    #Not on Intel network
    pass

build_dir=Dir('.').abspath
origin_dir = Dir('.').srcnode().abspath
top_dir = Dir('#').abspath
origin_offset = os.path.relpath(origin_dir, top_dir)

tmp = os.path.realpath(os.path.join(top_dir, env.get('PREFIX')))
updateValue(env, "PREFIX", tmp)
env.Alias('install', env.get('PREFIX'))

ins_root = '$PREFIX'

target_prefix = env.get("TARGET_PREFIX")
if target_prefix:
    target_prefix = os.path.realpath(os.path.join(top_dir, target_prefix))
    updateValue(env, "TARGET_PREFIX", target_prefix)

prebuilt_prefix = env.get("PREBUILT_PREFIX", None)
if prebuilt_prefix:
    #resolve a link if applicable
    prebuilt_prefix = os.path.realpath(os.path.join(top_dir, prebuilt_prefix))
    updateValue(env, "PREBUILT_PREFIX", prebuilt_prefix)

src_prefix = env.get("SRC_PREFIX", None)
if src_prefix:
    #resolve a link if applicable
    src_prefix = os.path.realpath(os.path.join(top_dir, src_prefix))
    updateValue(env, "SRC_PREFIX", src_prefix)

def run_commands(commands, subdir=None):
    retval = True
    global real_env
    old=os.getcwd()
    if subdir:
        os.chdir(subdir)
    print "Running commands in %s"%os.getcwd()
    for command in commands:
        print "RUN: %s"%command
        if subprocess.call(command, shell=True, env=real_env) != 0:
            retval = False
            break
    if subdir:
        os.chdir(old)
    return retval

#A class for retrieving code from a git repo
class git_repo:
    def __init__(self,
                 url,
                 has_submodules = False):
        self.url = url
        self.has_submodules = has_submodules

    def get(self, subdir):
        commands = ["git clone %s %s"%(self.url, subdir)]
        if not run_commands(commands):
            print "Could not get %s sources"%subdir
            Exit(-1)
        if self.has_submodules:
            commands = ["git submodule init",
                        "git submodule update"]
            if not run_commands(commands, subdir=subdir):
                print "Could not get %s submodules"%subdir
                Exit(-1)

class web:
    def __init__(self,
                 url):
        self.url = url

    def get(self, subdir):
        basename = os.path.basename(self.url)

        commands = ["rm -rf %s"%subdir]
        if not os.path.exists(basename):
            commands.append("curl -O %s"%self.url)

        if not run_commands(commands):
            print "Could not get %s sources"%subdir
            Exit(-1)

        if self.url.endswith(".tar.gz") or self.url.endswith(".tgz"):
            tf = tarfile.open(basename, 'r:gz')
            members = tf.getnames()
            prefix = os.path.commonprefix(members)
            tf.extractall()
            if not run_commands(["mv %s %s"%(prefix, subdir)]):
                print "Could not get %s sources"%subdir
                Exit(-1)
        else:
            print "Don't know how to get %s"%self.url
            Exit(-1)


class component:
    def __init__(self,
                 env,
                 subdir,
                 retriever,
                 build_commands,
                 expected_targets,
                 libs=[],
                 extra_lib_path = [],
                 extra_include_path = [],
                 out_of_source_build = False):
        global top_dir, build_dir, origin_dir
        global opts
        self.libs = libs
        self.env = env
        self.subdir = subdir
        self.retriever = retriever
        self.source_path = os.path.join(build_dir, subdir)
        self.crc = os.path.join(build_dir, ".%s.crc"%subdir)
        self.local_install = os.path.join(origin_dir,
                                         "objects",
                                         subdir)
        self.target_install = None
        if target_prefix:
            self.target_install = os.path.join(target_prefix, subdir)
        self.lib_path = ["lib"]
        self.include_path = ["include"]
        for path in extra_lib_path:
            self.lib_path.append(path)
        for path in extra_include_path:
            self.include_path.append(path)
        replacement = self.local_install
        if os.path.islink(self.local_install):
            os.unlink(self.local_install)
        try:
            os.makedirs(self.local_install)
        except:
            pass
        if not os.path.exists(self.local_install):
            print "Could not create %s"%self.local_install
            Exit(-1)
        if self.target_install:
            replacement = self.target_install
            if not os.path.islink(self.local_install):
                run_commands(["rm -rf %s"%(self.local_install)])
            #Add a command to link the directory
            build_commands.append("ln -sfn %s %s"%(self.target_install,
                                                 self.local_install))
        temp = map(lambda x: x.replace("$LOCAL_INSTALL",
                                  replacement), build_commands)
        self.rel_ins = os.path.join(build_dir, "objects", subdir)
        self.expected_targets = map(lambda x: os.path.join(self.local_install,
                                x),
                                expected_targets)
        self.src_opt = "%s_SRC"%subdir.upper()
        opts.Add(PathVariable(self.src_opt,
                              'Alternate path for %s source'%subdir,
                              None,
                              PathVariable.PathIsDir))
        opts.Update(self.env)
        if out_of_source_build:
            self.build_path = os.path.join(build_dir, "%s.build"%self.subdir)
            os.system("mkdir -p %s"%self.build_path)
            self.build_commands = map(lambda x: x.replace("$LOCAL_SRC",
                                      os.path.join("..", self.subdir)), temp)
        else:
            self.build_path = self.source_path
            self.build_commands = map(lambda x: x.replace("$LOCAL_SRC",
                                      self.subdir), temp)
        all_deps.add(self)

    def get(self):
        def_path = None
        if src_prefix and not isinstance(self.retriever, web):
            #Don't use src_prefix for web components
            def_path = os.path.join(src_prefix, self.subdir)
        def_path = self.env.get(self.src_opt, def_path)

        if def_path:
            if not os.path.exists(def_path):
                print "%s not found at %s"%(self.subdir, def_path)
                Exit(-1)
            def_path = os.path.realpath(os.path.join(top_dir, def_path))
            updateValue(self.env, self.src_opt, def_path)
            #Use the specified location
            print "Using %s source from %s"%(self.subdir, def_path)
            if not os.path.islink(self.source_path) or \
              os.path.realpath(self.source_path) != def_path:
                if not run_commands(["rm -rf %s"%self.source_path,
                                "ln -s %s %s"%(def_path, self.source_path)]):
                    print "Could not link %s sources"%self.subdir
                    Exit(-1)
        else:
            #Source code is retrieved using retriever
            print "Using repository for %s"%self.subdir
            if os.path.islink(self.source_path):
                os.system("rm %s"%self.source_path)
            if not os.path.exists(self.source_path):
                print "Downloading sources for %s"%self.subdir
                self.retriever.get(self.source_path)

    def calculate_crc(self):
        new_crc = ""
        for root, dirs, files in os.walk(self.subdir):
            for f in files:
                 (base, ext) = os.path.splitext(f)
                 #not fool proof but may be good enough
                 if ext in ['.c', '.h', '.cpp',
                            '.cc', '.hpp', '.ac',
                            '.in', '.py']:
                     with open(os.path.join(root, f), "r") as src:
                         new_crc += hashlib.md5(src.read()).hexdigest()
        return new_crc

    def has_changes(self):
        old_crc = ""
        try:
            with open(self.crc, "r") as crcfile:
                old_crc = crcfile.read()
        except:
            pass
        if old_crc != self.calculate_crc():
            return True
        return False

    def has_missing_targets(self):
        for target in self.expected_targets:
            if not os.path.exists(target):
                return True
        return False

    def build(self):
        if prebuilt_prefix:
            commands = ["rm -rf %s"%self.subdir,
                        "ln -s %s %s"%(os.path.join(prebuilt_prefix,
                                                    self.subdir),
                                                    self.subdir)]
            if not run_commands(commands,
                                subdir=os.path.join(origin_dir, "objects")):
               print "Could not link %s"%self.subdir
               Exit(-1)
        else:
            build_dir = os.path.join(self.build_path, self.subdir)
            if self.has_changes() or self.has_missing_targets():
                if not run_commands(self.build_commands,
                                    subdir=self.build_path):
                    print "Could not build %s"%self.subdir
                    Exit(-1)
        cwd = os.getcwd()
        if not os.path.exists(self.local_install):
             print "%s not found in %s"%(self.subdir, self.local_install)
             Exit(-1)
        os.chdir(self.local_install)
        self.targets = []
        for root, dirs, files in os.walk("."):
            for f in files:
                target = Glob(os.path.join(origin_offset, "objects",
                                           self.subdir, root, f))
                self.targets = self.targets + target
                self.env.Install(os.path.join(ins_root, root), target)
        os.chdir(cwd)
        #Add the relevant paths to the build environment
        for x in self.lib_path:
            self.env.Append(LIBPATH=[os.path.join(self.rel_ins, x)])
        for x in self.include_path:
            self.env.Append(CPPPATH=[os.path.join(self.rel_ins, x)])

        new_crc = self.calculate_crc()
        with open(self.crc, "w") as crcfile:
             crcfile.write(new_crc)

#A simple check to ensure that libevent is installed
if not config.CheckLib("libevent"):
    print "You need to install libevent development package to build iof."
    Exit(1)

libtoolize = "libtoolize"
rt = ["rt"]
bmi_build = ["./prepare"]

if env["PLATFORM"] == "darwin":
    libtoolize = "glibtoolize"
    rt = []
    bmi_build.append("./configure --enable-bmi-only --prefix=$LOCAL_INSTALL")
    bmi_lib = "lib/libbmi.a"
else:
    bmi_build.append("./configure --enable-shared --enable-bmi-only --prefix=$LOCAL_INSTALL")
    bmi_lib = "lib/libbmi.so"

bmi_build += ["make",
          "make install"]

bmi = component(env,
                  "bmi",
                  git_repo("http://git.mcs.anl.gov/bmi.git"),
                  bmi_build,
                  [bmi_lib])

openpa = component(env,
                  "openpa",
                  git_repo("http://git.mcs.anl.gov/radix/openpa.git"),
                  [libtoolize,
                  "./autogen.sh",
                  "./configure --prefix=$LOCAL_INSTALL",
                  "make",
                  "make install"],
                  ["lib/libopa.a"])

mercury = component(env,
                 "mercury",
                 git_repo("ssh://review.whamcloud.com:29418/daos/mercury",
                          True),
                 ["cmake -DOPA_LIBRARY=%s/lib/libopa.a "
                 "-DOPA_INCLUDE_DIR=%s/include/ "
                 "-DBMI_LIBRARY=%s/%s "
                 "-DBMI_INCLUDE_DIR=%s/include/ "
                 "-DCMAKE_INSTALL_PREFIX=$LOCAL_INSTALL "
                 "-DBUILD_EXAMPLES=ON -DMERCURY_BUILD_HL_LIB=ON "
                 "-DMERCURY_USE_BOOST_PP=ON -DNA_USE_BMI=ON "
                 "-DBUILD_TESTING=ON -DBUILD_DOCUMENTATION=OFF "
                 "-DBUILD_SHARED_LIBS=ON "
                 "$LOCAL_SRC"%(openpa.local_install, openpa.local_install,
                               bmi.local_install, bmi_lib, bmi.local_install),
                 "make",
                 "make install"],
                 ["lib/libmercury%s"%env.subst('$SHLIBSUFFIX')],
                 libs=['mercury', 'na', 'bmi',
                       'mercury_util', 'pthread',
                       'mchecksum'] + rt,
                 extra_include_path = [os.path.join("include", "na")],
                 out_of_source_build = True)

hwloc = component(env,
                  "hwloc",
                  web("https://www.open-mpi.org/software/hwloc/v1.11/downloads/hwloc-1.11.2.tar.gz"),
                  ["./configure --prefix=$LOCAL_INSTALL",
                  "make",
                  "make install"],
                  ["include/hwloc.h"])

pmix = component(env,
                "pmix",
                git_repo("https://github.com/pmix/master"),
                ["./autogen.sh",
                "./configure --with-platform=optimized "
                "--prefix=$LOCAL_INSTALL "
                "--with-libevent=/usr "
                "--with-hwloc=%s"%(hwloc.local_install),
                "make",
                "make install"],
                ["lib/libpmix%s"%env.subst("$SHLIBSUFFIX"),
                 "include/pmix.h"],
                libs=['pmix'])

ompi = component(env,
                "ompi",
                git_repo("https://github.com/open-mpi/ompi"),
                ["./autogen.pl --no-ompi --no-oshmem",
                "./configure --prefix=$LOCAL_INSTALL --with-pmix=%s "
                "--disable-mpi-fortran --with-libevent=/usr "
                "--with-hwloc=%s"%(pmix.local_install,
                                   hwloc.local_install),
                "make",
                "make install"],
                ["lib/libopen-rte%s"%env.subst("$SHLIBSUFFIX")])

Export('all_deps ins_root')
