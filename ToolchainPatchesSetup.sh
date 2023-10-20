#!/bin/bash

# Download the toolchains.zip file
wget https://mpl1.androidfilehost.com/dl/Ouhq4rv98FHBKe3Kux9XyQ/1697873134/16385555061192786590/toolchains.zip

# Unzip the toolchains.zip file
unzip toolchains.zip

# Clone the Kali NetHunter kernel build scripts
git clone https://gitlab.com/kalilinux/nethunter/build-scripts/kali-nethunter-kernel

# Copy the patches folder from kali-nethunter-kernel to the kernel source directory
cp -r kali-nethunter-kernel/patches .

# Change directory to kali-nethunter-kernel
cd kali-nethunter-kernel

# Execute the build.sh script
./build.sh

