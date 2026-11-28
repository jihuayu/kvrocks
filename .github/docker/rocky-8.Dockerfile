# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

FROM rockylinux:8

RUN dnf install -y epel-release \
    && dnf config-manager --set-enabled powertools \
    && dnf install -y git gcc-toolset-12 autoconf automake libtool make python3 python3-pip \
      openssl-devel which cmake wget zstd \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# RPM owns /usr/bin/gcc. Put the toolset alternatives earlier on PATH.
RUN update-alternatives --install /usr/local/bin/gcc gcc /opt/rh/gcc-toolset-12/root/usr/bin/gcc 100 \
    && update-alternatives --install /usr/local/bin/g++ g++ /opt/rh/gcc-toolset-12/root/usr/bin/g++ 100 \
    && update-alternatives --install /usr/local/bin/cc cc /opt/rh/gcc-toolset-12/root/usr/bin/gcc 100 \
    && update-alternatives --install /usr/local/bin/c++ c++ /opt/rh/gcc-toolset-12/root/usr/bin/g++ 100 \
    && pip3 install --no-cache-dir redis==4.3.6

RUN --mount=type=bind,source=install-redis.sh,target=/tmp/install-redis.sh \
    sh /tmp/install-redis.sh
