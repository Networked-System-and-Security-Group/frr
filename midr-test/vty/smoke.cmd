enable
show midr events
midr topology node upsert 1.1.1.1 group 100 transport 10.0.0.1 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 4294967296 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 1200 loss-ppm 10 available-bandwidth-kbps 900 version 4294967296 ifindex 0 seqno 4294967297 timestamp-ms 4294967298
show midr topology nodes
show midr topology links
show midr events
exit
