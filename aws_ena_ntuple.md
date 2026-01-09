# AWS ENA Flow Steering Configuration Guide

This guide documents the procedure to configure manual Flow Steering (ntuple filtering) on an AWS EC2 instance using the Elastic Network Adapter (ENA). The goal is to isolate traffic from a specific Source IP and force it into a dedicated CPU queue (Queue 3).

## 1. Prerequisites
* **Instance Type:** You must use a supported instance type (e.g., `c5.xlarge`, `m5.2xlarge`, or larger).
    * *Reason:* Smaller instances (like `t3` or `m5.large`) have `ntuple-filters` hard-locked to `off` and do not support manual steering.
* **Interface Name:** This guide uses `enp39s0`. Replace this with your actual interface name (e.g., `eth0`, `ens5`) if different.

---

## 2. Verify Ntuple Support
Before configuring rules, confirm that the hardware supports Flow Steering and that the driver feature is enabled.

**Command:**
```bash
ethtool -k enp39s0 | grep ntuple
```

Expected output: 
```
ntuple-filters: on
```

## 3. Isolate Queue 3 (RSS Weight Configuration)
By default, the ENA uses a Toeplitz hash to distribute all incoming traffic across all available queues. To reserve Queue 3 exclusively for our specific target IP, we must remove it from the general rotation.

We do this by setting the RSS indirection table weights. Assuming a 4-queue system, we assign weights of 1 to Queues 0-2, and 0 to Queue 3.

**Command:**
```bash
# Syntax: ethtool -X <interface> weight <q0> <q1> <q2> <q3>
sudo ethtool -X enp39s0 weight 1 1 1 0
```

## 4. Add Flow Steering Rule
We will now program the NIC to bypass the RSS hash for a specific IP address and direct it explicitly to Queue 3.

Target IP: 192.0.2.55 (Replace with your actual Source IP)

Protocol: tcp4

Destination: Queue 3 (action 3)

Rule ID: 2 (loc 2)

**Command:**
```bash
sudo ethtool -N enp39s0 flow-type tcp4 src-ip 192.0.2.55 action 3 loc 2
```

**Verify Command:**
```bash
ethtool -u enp39s0
Filter: 2
    Rule Type: TCP over IPv4
    Src IP: 192.0.2.55
    Dest IP: 0.0.0.0
    TOS: 0x0
    Action: Direct to queue 3
```

## 5 Verify Traffic (The Traffic Test)

Setup server to receive traffic on the host we did above

```bash
nc -l -k 5000 > /dev/null
```

On a client that has ip matched to rule 4:
```bash
yes | nc <SERVER_IP> 5000
```

Command that will show packet count increase:
```bash
watch -n 1 "ethtool -S enp39s0 | grep queue_3_rx_cnt"
```
