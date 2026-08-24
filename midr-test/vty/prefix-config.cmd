show midr prefix summary
configure terminal
route-map EXPORT-MIDR-V4 permit 10
exit
route-map EXPORT-MIDR-V6 permit 10
exit
router bgp 65000
neighbor 192.0.2.2 remote-as 65001
neighbor 2001:db8::2 remote-as 65002
midr group-prefix takeover-delay-ms 2500
address-family ipv4 unicast
midr prefix-export max-as-path-length 2
midr prefix-export route-map EXPORT-MIDR-V4
exit-address-family
address-family ipv6 unicast
midr prefix-export max-as-path-length 3
midr prefix-export route-map EXPORT-MIDR-V6
exit-address-family
end
show running-config
show midr prefix summary
configure terminal
router bgp 65000
no midr group-prefix takeover-delay-ms
address-family ipv4 unicast
no midr prefix-export max-as-path-length
no midr prefix-export route-map
exit-address-family
end
show running-config
exit
