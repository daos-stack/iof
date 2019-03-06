#!/usr/bin/groovy
/* Copyright (C) 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

def arch="-Linux"
def sanitized_JOB_NAME = JOB_NAME.toLowerCase().replaceAll('/', '-').replaceAll('%2f', '-')

pipeline {
    agent { label 'lightweight' }

    triggers {
        cron(env.BRANCH_NAME == 'master' ? 'H 0 * * *' : '')
    }

    environment {
        GITHUB_USER = credentials('daos-jenkins-review-posting')
        BAHTTPS_PROXY = "${env.HTTP_PROXY ? '--build-arg HTTP_PROXY="' + env.HTTP_PROXY + '" --build-arg http_proxy="' + env.HTTP_PROXY + '"' : ''}"
        BAHTTP_PROXY = "${env.HTTP_PROXY ? '--build-arg HTTPS_PROXY="' + env.HTTPS_PROXY + '" --build-arg https_proxy="' + env.HTTPS_PROXY + '"' : ''}"
        UID=sh(script: "id -u", returnStdout: true)
        BUILDARGS = "--build-arg UID=$env.UID $env.BAHTTP_PROXY $env.BAHTTPS_PROXY"
    }

    options {
        // preserve stashes so that jobs can be started at the test stage
        preserveStashes(buildCount: 5)
        timestamps ()
    }

    stages {
        stage('Pre-build') {
            parallel {
                stage('checkpatch') {
                    agent {
                        dockerfile {
                            filename 'Dockerfile.centos:7'
                            dir 'utils/docker'
                            label 'docker_runner'
                            additionalBuildArgs "-t ${sanitized_JOB_NAME}-centos7 " + '$BUILDARGS'
                        }
                    }
                    steps {
                        checkPatch user: GITHUB_USER_USR,
                                   password: GITHUB_USER_PSW
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'pylint.log', allowEmptyArchive: true
                        }
                    }
                }
            }
        }
        stage('Build') {
            // abort other builds if/when one fails to avoid wasting time
            // and resources
            failFast true
            parallel {
                stage('Build on CentOS 7') {
                    agent {
                        dockerfile {
                            filename 'Dockerfile.centos:7'
                            dir 'utils/docker'
                            label 'docker_runner'
                            additionalBuildArgs "-t ${sanitized_JOB_NAME}-centos7 " + '$BUILDARGS'
                        }
                    }
                    steps {
                        sh(script: """#!/bin/sh
set -x
set -e
rm -rf install/ deps/ iof/ .build-Vars*
BASE_DIR=`pwd`
cd /tmp/
cp -a \$BASE_DIR/ iof
cd iof
git clean -dfx
cp -a \$BASE_DIR/scons_local/ scons_local
find .
scons TARGET_PREFIX=\${BASE_DIR}/deps PREFIX=\${BASE_DIR}/iof --build-deps=yes
scons install
cp .build_vars-Linux.* \${BASE_DIR}/iof
""",
                         label: 'Try and build in tmpfs')
                        stash name: 'CentOS-install', includes: 'deps/**,iof/**'
                        stash name: 'CentOS-build-vars', includes: ".build_vars${arch}.*"
                    }
                    post {
                        always {
                            node('lightweight') {
                                recordIssues enabledForFailure: true,
                                             aggregatingResults: true,
                                             id: "analysis-centos7",
                                             tools: [ gcc4(), cppCheck() ],
                                             filters: [excludeFile('.*\\/_build\\.external-Linux\\/.*'),
                                                       excludeFile('_build\\.external-Linux\\/.*')]
                            }
                        }
                        success {
                            sh "rm -rf _build.external${arch}"
                        }
                    }
                }
                stage('Build on Ubuntu 18.04') {
                    when { branch 'master' }
                    agent {
                        dockerfile {
                            filename 'Dockerfile.ubuntu:18.04'
                            dir 'utils/docker'
                            label 'docker_runner'
                            additionalBuildArgs "-t ${sanitized_JOB_NAME}-ubuntu18.04 " + '$BUILDARGS'
                        }
                    }
                    steps {
                        sconsBuild clean: "_build.external${arch}"
                    }
                    post {
                        always {
                            node('lightweight') {
                                recordIssues enabledForFailure: true,
                                             aggregatingResults: true,
                                             id: "analysis-ubuntu18",
                                             tools: [ gcc4(), cppCheck() ],
                                             filters: [excludeFile('.*\\/_build\\.external-Linux\\/.*'),
                                                       excludeFile('_build\\.external-Linux\\/.*')]
                            }
                        }
                        success {
                            sh "rm -rf _build.external${arch}"
                        }
                    }
                }
            }
        }
        stage('Test') {
            parallel {
                stage('Single node') {
                    agent {
                        label 'ci_vm1'
                    }
                    steps {
                        provisionNodes NODELIST: env.NODELIST,
                           node_count: 1,
                           snapshot: true
                        runTest stashes: [ 'CentOS-install', 'CentOS-build-vars' ],
                                script: """set -x
                                    cd iof
                                    . ./.build_vars-Linux.sh
                                    CART_BASE=\${SL_PREFIX%/install*}
                                    NODELIST=$nodelist
                                    NODE=\${NODELIST%%,*}
                                    trap 'set +e; set -x; ssh -i ../ci_key jenkins@\$NODE "set -ex; sudo umount \$CART_BASE"' EXIT
                                    ssh -i ../ci_key jenkins@\$NODE "set -x
                                        set -e
                                        sudo mkdir -p \$CART_BASE
                                        sudo mount -t nfs \$HOSTNAME:\$PWD \$CART_BASE
                                        cd \$CART_BASE
                                        ln -s /usr/bin/fusermount iof/bin/fusermount3
                                        pip3.4 install --user tabulate
                                        nosetests-3.4 --exe --with-xunit"
                                    exit 0
                                    """,
                                junit_files: "nosetests.xml"
                    }
                    post {
                        always {
                            junit 'nosetests.xml'
                            archiveArtifacts artifacts: 'test/output/Testlocal/*/*.log'
                        }
                    }
                }
            stage('Fault injection') {
                agent {
                    label 'ci_vm1'
                }
                options {
                    timeout(time: 60, unit: 'MINUTES')
                }
                steps {
                    provisionNodes NODELIST: env.NODELIST,
                        node_count: 1,
                        snapshot: true
                    runTest stashes: [ 'CentOS-install', 'CentOS-build-vars' ],
                        script: """set -x
                            cd iof
                            . ./.build_vars-Linux.sh
                            CART_BASE=\${SL_PREFIX%/install*}
                            NODELIST=$nodelist
                            NODE=\${NODELIST%%,*}
                            trap 'set +e; set -x; ssh -i ../ci_key jenkins@\$NODE "set -ex; sudo umount \$CART_BASE"' EXIT
                            ssh -i ../ci_key jenkins@\$NODE "set -x
                                set -e
                                sudo mkdir -p \$CART_BASE
                                sudo mount -t nfs \$HOSTNAME:\$PWD \$CART_BASE
                                cd \$CART_BASE
                                ln -s /usr/bin/fusermount iof/bin/fusermount3
                                pip3.4 install --user tabulate
                                ./test/iof_test_alloc_fail.py"
                            """
                    publishValgrind (
                        failBuildOnInvalidReports: true,
                        failBuildOnMissingReports: true,
                        failThresholdDefinitelyLost: '0',
                        failThresholdInvalidReadWrite: '0',
                        failThresholdTotal: '0',
                        pattern: '**/*.memcheck',
                        publishResultsForAbortedBuilds: false,
                        publishResultsForFailedBuilds: false,
                        sourceSubstitutionPaths: '',
                        unstableThresholdDefinitelyLost: '',
                        unstableThresholdInvalidReadWrite: '',
                        unstableThresholdTotal: ''
                        )
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: '**/*.log,**/*.memcheck'
                        }
                    }
                }
            }
        }
    }
}
