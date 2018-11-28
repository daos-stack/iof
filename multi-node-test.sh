#!/bin/bash

set -ex -o pipefail

# shellcheck disable=SC1091
if [ -f .localenv ]; then
    # read (i.e. environment, etc.) overrides
    . .localenv
fi

HOSTPREFIX=${HOSTPREFIX-${HOSTNAME%%.*}}
NFS_SERVER=${NFS_SERVER:-$HOSTPREFIX}

trap 'echo "encountered an unchecked return code, exiting with error"' ERR

# shellcheck disable=SC1091
. .build_vars-Linux.sh

# shellcheck disable=SC2154
trap 'set +e
i=5
# due to flakiness on wolf-53, try this several times
while [ $i -gt 0 ]; do
    pdsh -R ssh -S -w ${HOSTPREFIX}vm[1-9] "set -x
    x=0
    while [ \$x -lt 30 ] &&
          grep $DAOS_BASE /proc/mounts &&
          ! sudo umount $DAOS_BASE; do
        ps axf
        sleep 1
        let x+=1
    done
    sudo rmdir $DAOS_BASE || find $DAOS_BASE || true" 2>&1 | dshbak -c
    if [ \${PIPESTATUS[0] = 0 ]; then
        i=0
    fi
    let i-=1
done' EXIT

DAOS_BASE=${SL_OMPI_PREFIX%/install/*}
if ! pdsh -R ssh -S -w "${HOSTPREFIX}"vm[1-9] "set -ex
ulimit -c unlimited
sudo mkdir -p $DAOS_BASE
sudo ed <<EOF /etc/fstab
\\\$a
$NFS_SERVER:$PWD $DAOS_BASE nfs defaults 0 0 # added by ftest.sh
.
wq
EOF
sudo mount $DAOS_BASE

# TODO: package this in to an RPM
pip3 install --user tabulate

df -h" 2>&1 | dshbak -c; then
    echo "Cluster setup (i.e. provisioning) failed"
    exit 1
fi

echo "hit enter to continue"
#read -r
#exit 0

cat <<EOF > install/Linux/TESTING/scripts/iof_fio_main.cfg
{
    "host_list": ["${HOSTPREFIX}vm2", "${HOSTPREFIX}vm3"],
    "test_mode": "littleChief"
}
EOF
cp install/Linux/TESTING/scripts/iof_{fio,ior}_main.cfg
cp install/Linux/TESTING/scripts/iof_{fio,iozone}_main.cfg
cp install/Linux/TESTING/scripts/iof_{fio,mdtest}_main.cfg

rm -rf install/Linux/TESTING/testLogs/
rm -f  install/Linux/bin/fusermount3
ln -s "$(command -v fusermount)" install/Linux/bin/fusermount3

# shellcheck disable=SC2029
if ! ssh "${HOSTPREFIX}"vm1 "set -ex
ulimit -c unlimited
cd $DAOS_BASE

# now run it!
pushd install/Linux/TESTING
python3 test_runner config=scripts/iof_fio_main.cfg scripts/iof_multi_two_node.yml
mv testLogs/testRun{,-fio}
python3 test_runner config=scripts/iof_ior_main.cfg scripts/iof_multi_two_node.yml
mv testLogs/testRun{,-ior}
python3 test_runner config=scripts/iof_iozone_main.cfg scripts/iof_multi_two_node.yml
mv testLogs/testRun{,-iozone}
python3 test_runner config=scripts/iof_mdtest_main.cfg scripts/iof_m_two_node.yml
mv testLogs/testRun{,-mdtest}


ls -l
exit \$rc"; then
    rc=${PIPESTATUS[0]}
else
    rc=0
fi

if false; then
# collect the logs
if ! rpdcp -R ssh -w "${HOSTPREFIX}"vm[1-9] \
    /tmp/Functional_"$TEST_TAG"/\*daos.log "$PWD"/; then
    echo "Copying daos.logs from remote nodes failed"
    # pass
fi
ls -l
fi
exit "$rc"
