#!/bin/sh

if [ ! -d .git/ ]; then
	echo UNKNOWN
	exit
fi

git describe
