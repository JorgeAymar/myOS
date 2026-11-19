# Networking (phase 12): pin the NIC to the classic Intel 82540EM ("e1000",
# 8086:100e) on QEMU user-mode networking (slirp: gw 10.0.2.2, DNS 10.0.2.3,
# DHCP lease 10.0.2.15). The driver also handles the q35 default 82574L.
QEMU_EXTRA += -nic user,model=e1000
