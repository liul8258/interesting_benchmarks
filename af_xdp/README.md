# Example showing how to do AF_XDP in AWS EC2
## Install requirements

```bash
sudo dnf install -y clang llvm make gcc m4 elfutils-libelf-devel git
sudo dnf install -y libbpf-devel elfutils-libelf-devel zlib-devel pkgconfig libpcap-devel

git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git
cd xdp-tools
make
sudo make install
sudo sh -c "echo /usr/local/lib > /etc/ld.so.conf.d/local.conf"
sudo ldconfig
```


## Build

```bash
make
```

## Run

```
Usage: ./xdp_client <IFNAME> <SRC_IP> <SRC_PORT> <DST_IP> <DST_PORT>
```
