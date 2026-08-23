enable
midr topology node upsert 1.1.1.1 group 100 version 100
configure terminal
router bgp 65000
bgp router-id 9.9.9.9
end
show midr topology sync
show midr topology nodes
midr topology node upsert 9.9.9.9 group 900 version 1
show midr topology nodes
show midr topology sync
show midr events
exit
