# Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#
# The build environment for release packages: Debian 10's glibc 2.28 and
# GCC 8, so that the module runs on every distribution from 2019 on. Debian
# 10 has left the mirrors for archive.debian.org.
FROM debian:10
RUN printf 'deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian-security buster/updates main\n' \
    > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends g++ make python3 \
 && rm -rf /var/lib/apt/lists/*
