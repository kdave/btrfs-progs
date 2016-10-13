#
# Copyright (C) 2016 Roman Lebedev.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License v2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public
# License along with this program; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 021110-1307, USA.
#

# docker build -t btrfs-progs .

# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! WARNING !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# !!! hub.docker.com will not auto-rebuild the image                        !!!
# !!! after making changes here, or if you just want to manually refresh    !!!
# !!! the image, you need to go to:                                         !!!
# https://hub.docker.com/r/lebedevri/btrfs-progs/~/settings/automated-builds/!!!
# !!!                            and press the "Trigger" button.            !!!
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! WARNING !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

FROM debian:testing
MAINTAINER Roman Lebedev <lebedev.ri@gmail.com>

# see https://github.com/docker-library/python/issues/13
ENV LANG C.UTF-8
ENV LC_ALL C.UTF-8
ENV LC_MESSAGES C.UTF-8
ENV LANGUAGE C.UTF-8

ENV DEBIAN_FRONTEND noninteractive

# Paper over occasional network flakiness of some mirrors.
RUN echo 'Acquire::Retries "10";' > /etc/apt/apt.conf.d/80retry

# Do not install recommended packages
RUN echo 'APT::Install-Recommends "false";' > /etc/apt/apt.conf.d/80recommends

# Do not install suggested packages
RUN echo 'APT::Install-Suggests "false";' > /etc/apt/apt.conf.d/80suggests

# Assume yes
RUN echo 'APT::Get::Assume-Yes "true";' > /etc/apt/apt.conf.d/80forceyes

# Fix broken packages
RUN echo 'APT::Get::Fix-Missing "true";' > /etc/apt/apt.conf.d/80fixmissin

# pls keep sorted :)
RUN rm -rf /var/lib/apt/lists/* && apt-get update && \
    apt-get install asciidoc debhelper e2fslibs-dev gcc git libacl1-dev \
    libattr1-dev libblkid-dev liblzo2-dev make pkg-config udev uuid-dev \
    xmlto zlib1g-dev && apt-get clean && rm -rf /var/lib/apt/lists/*

# i'd like to explicitly use ld.gold
# while it may be just immeasurably faster, it is known to cause more issues
# than traditional ld.bfd; plus, at this time, ld.gold seems like the future.
RUN dpkg-divert --add --rename --divert /usr/bin/ld.original /usr/bin/ld && \
    ln -s /usr/bin/ld.gold /usr/bin/ld
