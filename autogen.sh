#!/bin/bash

autoreconf --install

args="--prefix=/usr \
--libdir=/usr/lib \
--sysconfdir=/etc"

echo ./configure $args $@
./configure $args $@
