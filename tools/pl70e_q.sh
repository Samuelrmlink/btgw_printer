#!/bin/sh
# tools/pl70e_q.sh <write-char-id> <text-with-\r\n escapes> [listen-s]
A=${PL70E:-DD:0D:30:89:6C:C5}
H=$(printf "$2" | od -An -tx1 | tr -d ' \n')
python3 "$(dirname "$0")/btgw_op.py" gatt.write "{\"device\":\"$A\",\"id\":\"$1\",\"value\":\"$H\",\"response\":true}" --listen ${3:-3} --addr $A --evt char.value | python3 -c "
import sys,json
for l in sys.stdin:
  o=json.loads(l)
  if o.get('evt'): d=o['data']; print(d.get('uuid'), d.get('source'), bytes.fromhex(d.get('value','')))
  else: print(l.strip())
"
