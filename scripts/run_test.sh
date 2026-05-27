#!/bin/sh

echo "run_test.sh called from rc35" >> /var/config/ont_startup.log
(/var/config/ont_startup.sh) &
exit 0
