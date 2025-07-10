# SMARTFORWARDER
## Description

1)For compiling the application
```
gcc -o smartforwarder smartforwarder.c -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_pdump -lrte_log -lrte_net -lrte_mempool -mssse3
```

2) For Running the application
```
sudo ./smartforwarder -l 0-3 -n 4 -- -q 8 --no-mac-updating -P -T 20 -F

F : Packets Filtering(Based on Source IP Only 17.0.0.0/8 packets will be forwarded)
P : Promiscous mode
T : Time in secs 

```
