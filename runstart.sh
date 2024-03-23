#!/bin/sh
while [ ! -f /var/run/avahi-daemon/pid ]; do
  echo "Warning: avahi is not running, sleeping for 1 second before trying to start shairport-sync"
  sleep 1
done
while [ -f /var/run/avahi-daemon/pid ]; do
  sleep 5
done
