#!/bin/bash

automake_version=1.9

rm -f configure

autoheader
autoconf

rm -rf autom4te.cache

