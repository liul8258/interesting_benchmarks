# 1. Prerequisite: Instance Type
# We switched to a larger instance type (e.g., c5.xlarge or m5.2xlarge)
# because the previous instance type had "ntuple-filters" fixed to "off".

# 2. Verify Ntuple Support is Active
# Run this to confirm the hardware now supports flow steering.
# Result must show: "ntuple-filters: on"
ethtool -k enp39s0 | grep ntuple

# 3. Isolate Queue 3 (Remove it from RSS)
# We want to ensure ONLY our specific IP lands in Queue 3.
# By default, RSS (the random hash) might send random traffic there.
# We change the Indirection Table weights to 0 for Queue 3.
# Assuming 4 queues (0, 1, 2, 3), we give weights to 0-2 and exclude 3.
sudo ethtool -X enp39s0 weight 1 1 1 0

# 4. Add the Flow Steering Rule
# Instruct the NIC to bypass the RSS hash and force traffic from
# a specific IP (e.g., 192.0.2.55) directly to Queue 3.
# Note: We use an exact IP match because masks are often unsupported.
sudo ethtool -N enp39s0 flow-type tcp4 src-ip 192.0.2.55 action 3 loc 2

# Verify the rule was accepted:
ethtool -u enp39s0

# 5. Verify Traffic (The Test)
# Step A: On the Server, watch the packet counter for Queue 3.
# It should initially be static because we removed it from RSS.
watch -n 1 "ethtool -S enp39s0 | grep queue_3_rx_cnt"

# Step B: On the Client (Local Machine), generate TCP traffic.
# Use 'yes' to stream data to a port (ensure firewall allows it).
yes | nc <YOUR_SERVER_IP> 5000

# Result:
# The 'queue_3_rx_cnt' on the server should immediately start increasing,
# confirming that the hardware is successfully steering this specific IP.
