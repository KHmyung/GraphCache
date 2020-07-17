#!/bin/bash

large=$2

TestName=wiki-Vote
data=wiki-Vote

if [ $large -z ];then
	TestName=twitter_graph
	data=twitter_graph
fi

Thread=16
Mem=512M
base=/home/kh/codes/graph/FlashX
conf=$base/flash-graph/conf/run_test.txt
algo=$1
res=res/$TestName/$algo
parm=

set -e
#mkdir -p $res

cd $base/build; make -j64; cd $base;

sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
sleep 1

#sudo echo 1G > /cgroup/memory/test_algs/memory.limit_in_bytes

#perf record -g ./build/flash-graph/test-algs/test_algs $conf \
./build/flash-graph/test-algs/test_algs $conf \
		/mnt/graph/$data.adj /mnt/graph/$data.index $algo $parm > trace.log

vim trace.log
#pid=$!
#echo $pid >> /cgroup/memory/test_algs/tasks	

#free -m -s 1 | grep Mem > $res/mem2.log &
#free -m -s 1 | grep Swap > $res/swap2.log &
	
#wait $pid


#perf report --call-graph --stdio > $res/perf.log
#killall free
