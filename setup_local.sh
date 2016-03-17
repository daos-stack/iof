
if [ -z "$WORKSPACE" ]; then
  WORKSPACE=`pwd`
fi

os=`uname`
if [ "$os" = "Darwin" ]; then
    if [ -n "$DYLD_LIBRARY_PATH" ]; then
	export DYLD_LIBRARY_PATH=${WORKSPACE}/install/lib:$DYLD_LIBRARY_PATH
    else
	export DYLD_LIBRARY_PATH=${WORKSPACE}/install/lib
    fi
else
    if [ -n "$LD_LIBRARY_PATH" ]; then
	export LD_LIBRARY_PATH=${WORKSPACE}/install/lib:$LD_LIBRARY_PATH
    else
	export LD_LIBRARY_PATH=${WORKSPACE}/install/lib
    fi
fi
export PATH=${WORKSPACE}/install/bin:$PATH
