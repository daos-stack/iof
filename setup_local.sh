
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
# Allow overcommit of CPUs.
export OMPI_MCA_rmaps_base_oversubscribe=1

export OMPI_MCA_orte_abort_on_non_zero_status=0
