# ReplicatedBlockStore
MiniProject-3 for CS-739 distributed systems

1. Design & Implementation

1.1 Replication Strategy
src/server.cpp - This file code acts as server and backup both.
c_read() - Line 307 - Read function on both primary and backup
c_write() - Line 535 Write at primary server, calls s_write and s_commit on backup
s_write() - Line 643 - Write at backup server
s_commit() - Line 659 - Commit to file method for backup

1.2 Durability
Block state definition - Line
state initialization at primary (DISK) - Line 573
state initialization at backup (LOCKED) - Line 650
state transition at backup (LOCKED-> DISK)- Line 673

1.3 Crash Recovery Protocol
Reintegration strategy:
secondary_reintegration - recovery process starts at backup - Line 470
p_reintegration - consistency ensuring process at primary phase 1 - Line 402
p_reintegration_phase_two - consistency ensuring process at primary phase 2 - Line 431

hb_check() - load balancer heartbeat- Line 325
hb_tell() - Load balancer status update - Line 329 

2. Testing & Measurement

Run tests:
Server 1: ./bin/server -self ip:port -other ip:port -datafile ${file_path}
Server 2: ./bin/server -self ip:port -other ip:port -datafile ${file_path}
Load Balancer: ./bin/health_client -self lb_ip:lb_port -hosts server1_ip:port, server2_ip:port 

Client: 
2.1 Correctness ./bin/client -lb lb_ip:lb_port -compare 1/0 (match data at primary/backup/ not match)
2.2 Performance ./bin/test 1/2/3/4 -lb lb_ip:lb_port -compare 0
