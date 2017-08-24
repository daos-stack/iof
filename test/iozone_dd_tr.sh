#!/bin/sh

dd status=none bs=1 count=512k if=/dev/zero | tr '\0' '\72' > $1
