show midr sync
configure terminal
router bgp 65000
midr eor-timeout 5
end
show midr sync
show running-config
configure terminal
router bgp 65000
no midr eor-timeout
end
show midr sync
exit
