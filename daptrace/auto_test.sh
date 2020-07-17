#!/bin/bash

base=/home/kh/codes/graph/FlashX
conf=$base/flash-graph/conf/run_test.txt
algo=TopK
graph=twitter_graph

set -e

#
algo=wcc
for x in {2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do
#for x in {4,8,16,32,64}; do

	sed -i "10s/.*/cache_size=$x/g" $conf
#sed -i "28s/.*/threads=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $algo/$algo\_$x.log
done

cd $algo
sed -i '/Iter/d' *.log
cd ..

algo=scc
for x in {2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do
#for x in {4,8,16,32,64}; do

	sed -i "10s/.*/cache_size=$x/g" $conf
#sed -i "28s/.*/threads=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $algo/$algo\_$x.log
done

cd $algo
sed -i '/Iter/d' *.log
cd ..

algo=diameter
for x in {2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do
#for x in {4,8,16,32,64}; do

	sed -i "10s/.*/cache_size=$x/g" $conf
#sed -i "28s/.*/threads=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $algo/$algo\_$x.log
done

cd $algo
sed -i '/Iter/d' *.log
cd ..

algo=pagerank2
for x in {1.25G,2.5G,3.75G,5G,6.25G,7.5G,8.75G,10G,11.25G,12.5G}; do
#for x in {4,8,16,32,64}; do

	sed -i "10s/.*/cache_size=$x/g" $conf
#sed -i "28s/.*/threads=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $algo/$algo\_$x.log
done

cd $algo
sed -i '/Iter/d' *.log
cd ..
