#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Configuration
INTERFACE="eno1"

echo "--- Updating system and installing dependencies ---"
sudo apt update
sudo apt install -y meson python3-pyelftools libnuma-dev wget tar

echo "--- Configuring Hugepages ---"
# Sets 1024 pages of 2MB each (Total 2GB)
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

echo "--- Downloading and Extracting DPDK 25.11 ---"
wget -nc https://fast.dpdk.org/rel/dpdk-25.11.tar.xz
tar -xf dpdk-25.11.tar.xz
cd dpdk-25.11

echo "--- Building DPDK with Meson and Ninja ---"
meson setup build \
  -Dexamples=all \
  -Dbuildtype=release

cd build
ninja

echo "--- Installing DPDK ---"
sudo meson install
sudo ldconfig


#sudo ip addr flush dev enp3s0f0
#sudo ip link set enp3s0f0 down
#sudo dpdk-devbind.py -b vfio-pci --noiommu-mode 0000:03:00.0
