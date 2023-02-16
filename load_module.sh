#!/bin/bash

# Basic principle of script taken from:
# Linux Device Drivers, Third Edition by Jonathan Corbet, Alessandro Rubini, and Greg Kroah-Hartman


# First cleanup existing device
rm -rf /dev/hc-sr04

# Load the kernel module
insmod hc_sr04.ko

# Parse major driver number from /proc/devices output
drv_major=$(awk "/hc-sr04/ {print \$1}" /proc/devices)

mknod /dev/hc-sr04 c $drv_major 0
