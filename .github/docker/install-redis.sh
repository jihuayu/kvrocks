#!/bin/sh

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

set -eu

version=6.2.14
archive=redis-$version.tar.gz
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
cd "$work_dir"

wget -q -O "$archive" "https://download.redis.io/releases/$archive"
echo "34e74856cbd66fdb3a684fb349d93961d8c7aa668b06f81fd93ff267d09bc277  $archive" | sha256sum -c -
tar -xzf "$archive"
make -C "redis-$version" -j"$(nproc)" USE_JEMALLOC=no redis-cli redis-server
install -d /usr/local/bin
install -s "redis-$version/src/redis-cli" "redis-$version/src/redis-server" /usr/local/bin/
