enable
midr topology link upsert 1.1.1.1 2.2.2.2 id 1 local-address 10.0.0.1 remote-address 2001:db8::2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 2 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 0 loss-ppm 10 available-bandwidth-kbps 1000 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 3 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 0 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 4 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 1000000 available-bandwidth-kbps 1000 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id -1 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 18446744073709551616 local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 10x local-address 10.0.0.1 remote-address 10.0.0.2 rtt-us 100 loss-ppm 10 available-bandwidth-kbps 1000 version 1
show midr topology links
show midr events
exit
