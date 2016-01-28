import sys, os, hashlib, subprocess, socket
Import('env opts ins_root')

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

class component_group:
    def __init__(self):
        self.components = []
        self.targets = []

    def add(self, component):
        self.components.append(component)

    def build(self):
        for c in self.components:
            c.get()
        for c in self.components:
            c.build()
            self.targets = self.targets + c.targets

all_deps = component_group()

#checkout, if necessary, and build iof dependencies
real_env = {"PATH":"/bin:/usr/bin",
            "HOME":os.environ["HOME"]}
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
            sys.exit(-1)
        if self.has_submodules:
            commands = ["git submodule init",
                        "git submodule update"]
            if not run_commands(init, subdir=subdir):
                print "Could not get %s submodules"%subdir
                sys.exit(-1)

class web:
    def __init__(self,
                 url):
        self.url = url

    def get(self, subdir):
        basename = os.path.basename(self.url)
        if self.url.endswith(".tar.gz") or self.url.endswith(".tgz"):
            commands = ["wget %s"%self.url,
                        "mkdir -p %s; tar -zxvf %s -C %s --strip-components 1 "
                        "--overwrite"%(subdir, basename, subdir)]
        else:
            print "Don't know how to get %s"%self.url
            sys.exit(-1)

        if not run_commands(commands):
            print "Could not get %s sources"%subdir
            sys.exit(-1)

class component:
    def __init__(self,
                 env,
                 subdir,
                 retriever,
                 build_commands,
                 expected_targets,
                 extra_lib_path = [],
                 extra_include_path = [],
                 out_of_source_build = False):
        global top_dir, build_dir, origin_dir
        global opts
        all_deps.add(self)
        self.env = env
        self.subdir = subdir
        self.retriever = retriever
        self.source_path = os.path.join(build_dir, subdir)
        self.crc = os.path.join(build_dir, ".%s.crc"%subdir)
        self.local_install = os.path.join(origin_dir,
                                         "objects",
                                         subdir)
        self.lib_path = ["lib"]
        self.include_path = ["include"]
        for path in extra_lib_path:
            self.lib_path.append(path)
        for path in extra_include_path:
            self.include_path.append(path)
        try:
            os.mkdirs(self.local_install)
        except:
            pass
        temp = map(lambda x: x.replace("$LOCAL_INSTALL",
                                  self.local_install), build_commands)
        self.rel_ins = os.path.join(build_dir, "objects", subdir)
        self.expected_targets = map(lambda x: os.path.join(self.local_install,
                                x),
                                expected_targets)
        self.opt_name = "%s_SRC"%subdir.upper()
        opts.Add(PathVariable(self.opt_name,
                              'Alternate path for %s source'%subdir,
                              None,
                              PathVariable.PathIsDir))
        opts.Update(env)
        if out_of_source_build:
            self.build_path = os.path.join(build_dir, "%s.build"%self.subdir)
            os.system("mkdir -p %s"%self.build_path)
            self.build_commands = map(lambda x: x.replace("$LOCAL_SRC",
                                      os.path.join("..", self.subdir)), temp)
        else:
            self.build_path = self.source_path
            self.build_commands = map(lambda x: x.replace("$LOCAL_SRC",
                                      self.subdir), temp)

    def get(self):
        def_path = os.path.join(top_dir, "..", self.subdir)
        def_path = self.env.get(self.opt_name, def_path)

        if os.path.exists(def_path):
            #Use the specified location
            print "Using %s source from %s"%(self.subdir, def_path)
            if not os.path.islink(self.source_path) or \
              os.path.realpath(self.source_path) != os.path.realpath(def_path):
                if not run_commands(["rm -rf %s"%self.source_path,
                                "ln -s %s %s"%(def_path, self.source_path)]):
                    print "Could not link %s sources"%self.subdir
                    sys.exit(-1)
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
        if self.has_changes() or self.has_missing_targets():
            if not run_commands(self.build_commands, subdir=self.build_path):
                       print "Could not build %s"%self.subdir
                       sys.exit(-1)
        cwd = os.getcwd()
        os.chdir(self.local_install)
        self.targets = []
        for root, dirs, files in os.walk("."):
            for f in files:
                target = Glob(os.path.join("objects", self.subdir, root, f))
                self.targets = self.targets + target
                env.Install(os.path.join(ins_root, root), target)
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
config = Configure(env)
if not config.CheckLib("libevent"):
    print "You need to install libevent development package to build iof."
    Exit(1)

bmi = component(env,
                  "bmi",
                  git_repo("http://git.mcs.anl.gov/bmi.git"),
                  ["./prepare",
                  "./configure --enable-shared --prefix=$LOCAL_INSTALL",
                  "make",
                  "make install"],
                  ["lib/libbmi.so"])

openpa = component(env,
                  "openpa",
                  git_repo("http://git.mcs.anl.gov/radix/openpa.git"),
                  ["libtoolize",
                  "./autogen.sh",
                  "./configure --prefix=$LOCAL_INSTALL",
                  "make",
                  "make install"],
                  ["lib/libopa.a"])

mercury = component(env,
                 "mercury",
                 git_repo("ssh://review.whamcloud.com/daos/mercury",
                          True),
                 ["cmake -DOPA_LIBRARY=%s/lib/libopa.a "
                 "-DBMI_LIBRARY=%s/lib/libbmi.so "
                 "-DBMI_INCLUDE_DIR=%s/include/ "
                 "-DCMAKE_INSTALL_PREFIX=$LOCAL_INSTALL "
                 "-DBUILD_EXAMPLES=ON -DMERCURY_BUILD_HL_LIB=ON "
                 "-DMERCURY_USE_BOOST_PP=ON -DNA_USE_BMI=ON "
                 "-DBUILD_TESTING=ON -DBUILD_DOCUMENTATION=OFF "
                 "-DBUILD_SHARED_LIBS=ON "
                 "$LOCAL_SRC"%(openpa.local_install,
                               bmi.local_install, bmi.local_install),
                 "make",
                 "make install"],
                 ["lib/libmercury.so"],
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
                git_repo("https://github.com/open-mpi/ompi"),
                ["./autogen.sh",
                "./configure --with-platform=optimized "
                "--prefix=$LOCAL_INSTALL "
                "--with-libevent=/usr "
                "--with-hwloc=%s"%(hwloc.local_install),
                "make",
                "make install"],
                ["lib/libpmix.so",
                 "include/pmix.h"])

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
                ["lib/libopen-rte.so"])

Export('all_deps')
