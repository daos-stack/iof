
eval `./load_file.py iof.conf`

os=`uname`
if [ "$os" = "Darwin" ]; then
    if [ -n "$DYLD_LIBRARY_PATH" ]; then
	export DYLD_LIBRARY_PATH=${PREFIX}/lib:$DYLD_LIBRARY_PATH
    else
	export DYLD_LIBRARY_PATH=${PREFIX}/lib
    fi
fi

export PATH=${PREFIX}/bin:$PATH
