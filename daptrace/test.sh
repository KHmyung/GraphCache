#!/bin/bash

base=/home/kh/codes/graph/FlashX
testalgs=$base/build/flash-graph/test-algs/test_algs
conf=$base/flash-graph/conf/run_test.txt
algo=$1
file=$2
file_loc=/mnt/graph
parm="-i 5"

gdb --args $testalgs $conf $file_loc/$file.adj $file_loc/$file.index $algo $parm
