#!/bin/bash

set -e

#path=./pagerank2_st/
#
#cd $path
#for x in {1,2,4,6,8,10,12,13}; do
#	echo "$x" | python draw.py
#done
#cd ..
#
#path=./pagerank2_mt/
#
#cd $path
#for x in {4,8,12}; do
#	echo "$x" | python draw.py
#done
#cd ..
#
#path=./diameter/
#
#cd $path
#for x in {1,4,8,12,16,20,24,28}; do
#	echo "$x" | python draw.py
#done
#cd ..

path=./wcc/

cd $path
for x in {1,4,8,12,16,20,24,28}; do
	echo "$x" | python draw.py
done
cd ..
