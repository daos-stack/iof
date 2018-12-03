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

if [ "$1" = "2" ]; then
    test_runner_vm=$((${EXECUTOR_NUMBER:-0}*3+7))
    vm1="$((test_runner_vm+1))"
    vm2="$((test_runner_vm+2))"
    vmrange="$vm1-$vm2"
    test_runner_vm="vm$test_runner_vm"
    vm1="vm$vm1"
    vm2="vm$vm2"
elif [ "$1" = "5" ]; then
    test_runner_vm="vm1"
    vmrange="2-6"
fi

log_base_path="testLogs-${1}_node"

# shellcheck disable=SC2154
trap 'set +e
i=5
# due to flakiness on wolf-53, try this several times
while [ $i -gt 0 ]; do
    pdsh -R ssh -S -w "${HOSTPREFIX}$test_runner_vm,${HOSTPREFIX}vm[$vmrange]" "set -x
    x=0
    while [ \$x -lt 30 ] &&
          grep $DAOS_BASE /proc/mounts &&
          ! sudo umount $DAOS_BASE; do
        ps axf
        sleep 1
        let x+=1
    done
    sudo sed -i -e \"/added by multi-node-test-$1.sh/d\" /etc/fstab
    sudo rmdir $DAOS_BASE || find $DAOS_BASE || true" 2>&1 | dshbak -c
    if [ ${PIPESTATUS[0]} = 0 ]; then
        i=0
    fi
    let i-=1
done' EXIT

DAOS_BASE=${SL_OMPI_PREFIX%/install/*}
if ! pdsh -R ssh -S -w "${HOSTPREFIX}$test_runner_vm,${HOSTPREFIX}vm[$vmrange]" "set -ex
ulimit -c unlimited
sudo mkdir -p $DAOS_BASE
sudo ed <<EOF /etc/fstab
\\\$a
$NFS_SERVER:$PWD $DAOS_BASE nfs defaults 0 0 # added by multi-node-test-$1.sh
.
wq
EOF
if ! sudo mount $DAOS_BASE; then
    if [ \"\${HOSTNAME%%%%.*}\" = \"${HOSTPREFIX}$test_runner_vm\" ]; then
        # could be already mounted from another test running in parallel
        # let's see what that rc is
        echo \"mount rc: \${PIPESTATUS[0]}\"
    else
        exit \${PIPESTATUS[0]}
    fi
fi

# TODO: package this in to an RPM
pip3 install --user tabulate

df -h" 2>&1 | dshbak -c; then
    echo "Cluster setup (i.e. provisioning) failed"
    exit 1
fi

#echo "hit enter to continue"
#read -r
#exit 0

rm -f  install/Linux/bin/fusermount3
ln -s "$(command -v fusermount)" install/Linux/bin/fusermount3

# shellcheck disable=SC2029
if ! ssh "${HOSTPREFIX}$test_runner_vm" "set -ex
ulimit -c unlimited
cd $DAOS_BASE

# now run it!
pushd install/Linux/TESTING/
if [ \"$1\" = \"2\" ]; then
    cat <<EOF > scripts/iof_fio_main.cfg
{
    \"host_list\": [\"${HOSTPREFIX}${vm1}\", \"${HOSTPREFIX}${vm2}\"],
    \"test_mode\": \"littleChief\",
    \"log_base_path\": \"$log_base_path\"
}
EOF
    cp scripts/iof_{fio,ior}_main.cfg
    cp scripts/iof_{fio,iozone}_main.cfg
    cp scripts/iof_{fio,mdtest}_main.cfg

    rm -rf $log_base_path/
    python3 test_runner config=scripts/iof_fio_main.cfg \\
            scripts/iof_multi_two_node.yml || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
    mv $log_base_path/testRun{,-fio}
    find $log_base_path/testRun-fio -name subtest_results.yml \\
         -exec grep -Hi fail {} \\;
    python3 test_runner config=scripts/iof_ior_main.cfg \\
            scripts/iof_multi_two_node.yml || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
    mv $log_base_path/testRun{,-ior}
    find $log_base_path/testRun-ior -name subtest_results.yml \\
         -exec grep -Hi fail {} \\;
    python3 test_runner config=scripts/iof_iozone_main.cfg \\
            scripts/iof_multi_two_node.yml || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
    mv $log_base_path/testRun{,-iozone}
    find $log_base_path/testRun-iozone -name subtest_results.yml \\
         -exec grep -Hi fail {} \\;
    python3 test_runner config=scripts/iof_mdtest_main.cfg \\
            scripts/iof_multi_two_node.yml || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
    mv $log_base_path/testRun{,-mdtest}
    find $log_base_path/testRun-mdtest -name subtest_results.yml \\
         -exec grep -Hi fail {} \\;
elif [ \"$1\" = \"5\" ]; then
    cat <<EOF > scripts/iof_multi_five_node.cfg
{
    \"host_list\": [
        \"${HOSTPREFIX}vm2\",
        \"${HOSTPREFIX}vm3\",
        \"${HOSTPREFIX}vm4\",
        \"${HOSTPREFIX}vm5\",
        \"${HOSTPREFIX}vm6\"
    ],
    \"test_mode\": \"littleChief\",
    \"log_base_path\": \"$log_base_path\"
}
EOF
    rm -rf $log_base_path/
    python3 test_runner config=scripts/iof_multi_five_node.cfg \\
            scripts/iof_multi_five_node.yml || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
    find $log_base_path/testRun -name subtest_results.yml \\
         -exec grep -Hi fail {} \\;
fi
exit \$rc"; then
    rc=${PIPESTATUS[0]}
else
    rc=0
fi

{
    cat <<EOF
TestGroup:
    submission: $(TZ=UTC date)
    test_group: IOF_${1}-node
    testhost: $HOSTNAME
    user_name: jenkins
Tests:
EOF
    pwd >&2
    hostname >&2
    ls -ld install install/Linux install/Linux/TESTING >&2
    ls -l install/Linux/TESTING >&2
    df -h install/Linux/TESTING >&2
    find install/Linux/TESTING/$log_base_path -name subtest_results.yml \
         -print0 | xargs -0 cat
} > results_1.yml
cat results_1.yml

PYTHONPATH=scony_python-junit/ jenkins/autotest_utils/results_to_junit.py

if false; then
# collect the logs
if ! rpdcp -R ssh -w "${HOSTPREFIX}"vm[1,$vmrange] \
    /tmp/Functional_"$TEST_TAG"/\*daos.log "$PWD"/; then
    echo "Copying daos.logs from remote nodes failed"
    # pass
fi
ls -l
fi
exit "$rc"
