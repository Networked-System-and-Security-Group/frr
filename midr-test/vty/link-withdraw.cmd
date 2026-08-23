enable
midr topology link withdraw 1.1.1.1 2.2.2.2 id 99 version 5
midr topology link upsert 1.1.1.1 2.2.2.2 id 99 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 4
show midr topology links
show midr topology tombstones
show midr events
exit
