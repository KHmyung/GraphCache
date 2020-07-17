#!/bin/bash

algo=pagerank2
file=twitter_graph
#algo=pagerank2
#file=wiki-Vote

set -e

sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
sleep 1

sudo ./daptrace.py -o $file -n 500 -m 1000 "sudo ./test.sh $algo $file"
sleep 1

sudo ./scripts/report.py --data /$file --name $algo --vmax 4
