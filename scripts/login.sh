#!/bin/bash

echo "Starting Scratchbox 2..."
echo "Using sudo, type your password:"

sudo ./scratchbox/bin/chroot-uid $(pwd) /scratchbox/bin/sb2init

