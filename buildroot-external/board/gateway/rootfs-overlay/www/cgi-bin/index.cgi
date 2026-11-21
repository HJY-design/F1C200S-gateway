#!/bin/sh
case "$HTTP_HOST" in
  f1c200s-gateway.local|f1c200s-gateway.local:80)
    task_ip=$(ip -4 -o addr show dev wlan0 | awk '{split($4,a,"/");print a[1];exit}')
    if [ -n "$task_ip" ]; then
      printf 'Status: 302 Found\r\nLocation: http://%s/index.html\r\nCache-Control: no-store\r\n\r\n' "$task_ip"
      exit 0
    fi
    ;;
esac
printf 'Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n\r\n'
cat /www/index.html
