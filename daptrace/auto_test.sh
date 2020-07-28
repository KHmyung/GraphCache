#!/bin/bash

testname=apr_time
base=/home/kh/codes/graph/FlashX
conf=$base/flash-graph/conf/run_test.txt
algo=pagerank2
graph=twitter_graph
parm=

set -e

sed -i "28s/.*/threads=1/g" $conf

algo=wcc
mkdir -p $testname/$algo
for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1

	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th1.log
done

algo=wcc
mkdir -p $testname/$algo
sed -i "28s/.*/threads=16/g" $conf

for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th16.log
done

sed -i "28s/.*/threads=1/g" $conf

algo=scc
mkdir -p $testname/$algo
for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th1.log
done

algo=scc
mkdir -p $testname/$algo
sed -i "28s/.*/threads=16/g" $conf

for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th16.log
done

sed -i "28s/.*/threads=1/g" $conf

algo=diameter
mkdir -p $testname/$algo
for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th1.log
done

algo=diameter
mkdir -p $testname/$algo
sed -i "28s/.*/threads=16/g" $conf

for x in {128M,2.6G,5.2G,7.8G,10.4G,13G,15.6G,18.2G,20.8G,23.4G,26G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph > $testname/$algo/$algo\_$x\_th16.log
done

sed -i "28s/.*/threads=1/g" $conf

algo=pagerank2
mkdir -p $testname/$algo
for x in {128M,1.3G,2.6G,3.9G,5.2G,6.5G,7.8G,9.1G,10.4G,11.7G,13G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph '-i 5' > $testname/$algo/$algo\_$x\_th1.log
done

algo=pagerank2
mkdir -p $testname/$algo
sed -i "28s/.*/threads=16/g" $conf

for x in {128M,1.3G,2.6G,3.9G,5.2G,6.5G,7.8G,9.1G,10.4G,11.7G,13G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph '-i 5' > $testname/$algo/$algo\_$x\_th16.log
done

algo=cycle_triangle
mkdir -p $testname/$algo

for x in {1.3G,2.6G,3.9G,5.2G,6.5G,7.8G,9.1G,10.4G,11.7G,13G}; do

	sed -i "10s/.*/cache_size=$x/g" $conf

	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
	
	sudo ./test.sh $algo $graph -f > $testname/$algo/$algo\_$x\_th16.log
done
