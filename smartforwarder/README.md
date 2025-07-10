# SMARTFORWARDER
## Description

1)For compiling the application
```
gcc -o smartforwarder smartforwarder.c -lrte_eal -lrte_ethdev -lrte_mbuf -lrte_pdump -lrte_log
```

2) For Running the application
```
sudo ./smartforwarder -l 0-3 -n 4 -- -q 8 --no-mac-updating -P -T 20
```
