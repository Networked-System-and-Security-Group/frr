enable
midr topology node upsert 9.9.9.9 group 100 version 1
midr topology link upsert 9.9.9.9 2.2.2.2 id 1 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 1
show midr topology nodes
show midr topology links
show midr events
exit
