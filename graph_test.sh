#!/bin/bash

TestName=twt-scale-algo	##
Thread=16
Mem=512M
data=twitter_graph
base=/home/kh/codes/graph/FlashX
conf=$base/flash-graph/conf/run_test.txt ##
algo=pagerank2	##
#algo=betweenness
res=res/$TestName/$algo
parm=

set -e
mkdir -p $res

#for x in {1G,4G,8G,12G,16G,20G,24G,28G,32G}; do
for x in {4,8,16,32,64}; do
#for x in {256M,1G}; do

#	sed -i "10s/.*/cache_size=$x/g" $conf
sed -i "28s/.*/threads=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
#sudo echo 32G > /cgroup/memory/test_algs/memory.limit_in_bytes

	./build/flash-graph/test-algs/test_algs $conf /mnt/graph/$data.adj \
		/mnt/graph/$data.index $algo $parm > $res/result$x.log &
	pid=$!

#iostat 1 -m | grep nvme0n1 > $res/io$x.log &

#free -m -s 1 | grep Mem > $res/mem32G.log &
#free -m -s 1 | grep Swap > $res/swap32G.log &
	
#echo $pid >> /cgroup/memory/test_algs/tasks	
	wait $pid
#killall iostat

#killall free
done
