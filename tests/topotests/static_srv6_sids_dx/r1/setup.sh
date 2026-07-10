ip link add sr0 type dummy
ip link set sr0 up

sysctl -w net.vrf.strict_mode=1
