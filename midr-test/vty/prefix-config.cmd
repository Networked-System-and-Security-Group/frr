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
neighbor 192.0.2.2 midr external-prefix-source
midr prefix-export route-map EXPORT-MIDR-V4
midr prefix-export local-source network
midr prefix-export local-source connected
midr prefix-export local-source static
exit-address-family
address-family ipv6 unicast
neighbor 2001:db8::2 midr external-prefix-source
midr prefix-export route-map EXPORT-MIDR-V6
midr prefix-export local-source network
exit-address-family
end
show running-config
show midr prefix summary
configure terminal
router bgp 65000
no midr group-prefix takeover-delay-ms
address-family ipv4 unicast
no neighbor 192.0.2.2 midr external-prefix-source
no midr prefix-export route-map
no midr prefix-export local-source network
no midr prefix-export local-source connected
no midr prefix-export local-source static
exit-address-family
end
show running-config
exit
