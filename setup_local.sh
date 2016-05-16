
VARS_FILE=./.build_vars-`uname -s`.sh

if [ ! -f $VARS_FILE ]
then
    echo Build vars file $VARS_FILE does not exist
    echo Cannot continue
    exit 1
fi

. $VARS_FILE

os=`uname`
if [ "$os" = "Darwin" ]; then
    if [ -n "$DYLD_LIBRARY_PATH" ]; then
	export DYLD_LIBRARY_PATH=${SL_LD_LIBRARY_PATH}:$DYLD_LIBRARY_PATH
    else
	export DYLD_LIBRARY_PATH=${SL_LD_LIBRARY_PATH}
    fi
fi


if [ -z "$SL_PREFIX" ]
then
    SL_PREFIX=`pwd`/install
fi

export PATH=$SL_PREFIX/bin:${SL_OMPI_PREFIX}/bin:$PATH

# Set some ORTE runtime variables, use "orte-info --param all all" for help on
# individual options.

# Allow overcommit of CPUs.
export OMPI_MCA_rmaps_base_oversubscribe=1

# Prevent orte from killing the job on single process failure
export OMPI_MCA_orte_abort_on_non_zero_status=0

# Use /tmp for temporary files.  This is required for OS X.
export OMPI_MCA_orte_tmpdir_base=/tmp
