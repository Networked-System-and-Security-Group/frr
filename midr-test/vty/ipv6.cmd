enable
midr topology node upsert 1.1.1.1 group 600 transport 2001:db8::1 version 6
midr topology link upsert 1.1.1.1 2.2.2.2 id 6 local-address 2001:db8::1 remote-address 2001:db8::2 rtt-us 600 loss-ppm 60 available-bandwidth-kbps 6000 version 6
show midr topology nodes
show midr topology links
show midr events
exit
