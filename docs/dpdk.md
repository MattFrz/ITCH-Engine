# DPDK backend (optional)

DPDK is **not** a dependency of this project. `cmake -S . -B build` configures
and the whole engine builds, tests and benchmarks on a machine that has never
heard of it. Enabling the backend is an explicit choice, made on a host that
already has DPDK, a supported NIC, hugepages, and a port you are willing to
take away from the kernel.

## Status, stated plainly

The backend is written against the DPDK 21.11+ API and is **compile-gated and
unmeasured**. No DPDK hardware was available while writing it. It is a starting
point for a deployment, not a result - which is why there is not a single DPDK
latency number anywhere in this repository. Benchmark it yourself before
believing anything about it.

## What it changes, and what it does not

```
kernel path:  NIC -> IRQ -> driver -> kernel stack -> socket buffer
                -> recvmmsg() copy -> MoldUDP64 -> ITCH -> book

DPDK path:    NIC -> DMA into a hugepage mbuf -> rte_eth_rx_burst()
                -> skip Ethernet/IPv4/UDP headers in place
                -> MoldUDP64 -> ITCH -> book
```

Gone: the interrupt, the kernel stack traversal, the syscall, and the copy into
user space. The ITCH parser reads the bytes the NIC wrote.

Unchanged: MoldUDP64, the ITCH parser, `MarketEvent`, the book, the strategy
interface. That is the whole reason the backend sits behind
`NetworkBackend` - the receive path is the only thing that differs, and the
correctness work above it is shared.

## Requirements

* Linux, kernel 4.14+ (5.x recommended)
* DPDK 21.11 LTS or newer, with `pkg-config` able to find `libdpdk`
* A NIC with a DPDK poll-mode driver (Intel X710/E810, Mellanox
  ConnectX-4/5/6, Solarflare, and others - check the DPDK release notes for
  your part)
* Hugepages reserved
* IOMMU enabled (`intel_iommu=on iommu=pt`) or `vfio-pci` in no-IOMMU mode
* Root, or `vfio-pci` group permissions

## Setup

### 1. Install DPDK

Distribution package:

```bash
sudo apt install dpdk dpdk-dev libdpdk-dev     # Debian/Ubuntu
sudo dnf install dpdk dpdk-devel               # Fedora/RHEL
```

Or from source, which is what you want if you need a specific version:

```bash
git clone https://github.com/DPDK/dpdk.git && cd dpdk
git checkout v23.11
meson setup build && cd build && ninja && sudo ninja install
sudo ldconfig
pkg-config --modversion libdpdk    # must print a version
```

### 2. Reserve hugepages

```bash
# 2 MiB pages, 2 GiB total
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

Boot-time reservation is more reliable on a machine that has been up a while:

```
default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

### 3. Bind a NIC to a userspace driver

**This takes the interface away from the kernel.** Do not do it to the
interface you are connected over.

```bash
sudo modprobe vfio-pci
./usertools/dpdk-devbind.py --status
sudo ./usertools/dpdk-devbind.py --bind=vfio-pci 0000:03:00.0
```

To undo it:

```bash
sudo ./usertools/dpdk-devbind.py --bind=ixgbe 0000:03:00.0
```

### 4. Build with the backend enabled

```bash
cmake -S . -B build-dpdk -DCMAKE_BUILD_TYPE=Release -DITCH_ENABLE_DPDK=ON
cmake --build build-dpdk -j
```

If `pkg-config` cannot find DPDK, configuration fails with a message saying so
rather than silently producing a build without it.

### 5. Run

```bash
sudo ./build-dpdk/cpp/itch_live \
  --backend dpdk \
  --dpdk-eal -l --dpdk-eal 4 \
  --dpdk-eal --proc-type=primary \
  --dpdk-port 0 \
  --multicast-group <your group> \
  --port <your port> \
  --cpu 4 --lock-memory
```

EAL arguments are passed through verbatim, one per `--dpdk-eal`. Nothing here
invents a core mask, a PCI address or a memory channel count.

Without a DPDK build, `--backend dpdk` fails with an explanation rather than
falling back silently:

```
cannot open feed: this build has no DPDK backend. Reconfigure with
-DITCH_ENABLE_DPDK=ON on a host with DPDK >= 21.11, hugepages reserved and a
NIC bound to a userspace driver (see docs/dpdk.md), or use the default kernel
UDP backend.
```

## Implementation notes

* `receive()` frees the previous burst's mbufs at the start of the next call,
  which is why a `PacketView` is documented as valid only until the following
  `receive()`. The MoldUDP64 decoder consumes each packet fully before the next
  call, so this is safe by construction.
* Ethernet, IPv4 and UDP headers are skipped in place. IPv4 options are handled
  (IHL is read rather than assumed).
* Destination group and UDP port are filtered in software. On a NIC with
  `rte_flow` support, pushing that filter into hardware so unwanted traffic
  never reaches the core is the obvious next step and is not done yet.
* Promiscuous mode is enabled, because a multicast destination MAC is not the
  port's own address.
* `imissed` and `rx_nombuf` from `rte_eth_stats_get` are reported through the
  same `kernel_drops` counter the socket backend uses, so "the host fell
  behind" reads the same in both.
* NIC hardware timestamps are read from the mbuf when the driver supplies them
  and `--hardware-timestamps` is set.

## Before trusting any DPDK number

Measure it against the kernel path on the same host, same feed, same core:

```bash
# kernel
sudo ./build-dpdk/cpp/itch_live --backend udp  --multicast-group ... --port ... --cpu 4
# poll mode
sudo ./build-dpdk/cpp/itch_live --backend dpdk --dpdk-eal -l --dpdk-eal 4 ... --cpu 4
```

and compare `kernel_drops`, the message rate, and the receive-to-book latency
with hardware timestamps enabled. A DPDK backend that is not faster on your
hardware is complexity you do not need.
