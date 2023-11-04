#!/bin/bash

[[ $(pgrep mute-on-lock.sh | wc -l) -gt 2 ]] && exit 0

gdbus monitor -y -d org.freedesktop.login1 | \
  command grep -oP "(?<='LockedHint': <)[^>]*" --line-buffered | \
    while read line
    do
      pactl set-sink-mute @DEFAULT_SINK@ $line
    done
