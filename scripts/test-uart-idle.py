#!/usr/bin/env python3
"""Exercise the real UART helper against fake MMIO, including unrelated bits."""
from pathlib import Path
import json, os, subprocess, sys, tempfile
helper = Path(sys.argv[1]).resolve()
fake = """#!/usr/bin/python3
import json,os,sys
p=os.environ['GW_FAKE_REGS']
s=json.load(open(p)); a=sys.argv[1].lower()
if len(sys.argv)>3:
 if os.environ.get('GW_FAIL_WRITE'):sys.exit(1)
 s[a]=int(sys.argv[3],0);s['writes']=s.get('writes',0)+1
 json.dump(s,open(p,'w'))
else:print('0x%08x'%s[a])
"""
passed=[]
def run(name,pd,pe,action="apply",compatible="custom,f1c200s-gateway",expect=0,fail_write=False):
 with tempfile.TemporaryDirectory() as temp:
  d=Path(temp); (d/"devmem").write_text(fake); (d/"devmem").chmod(0o755)
  (d/"compatible").write_bytes(compatible.encode()+b"\0allwinner,suniv-f1c100s\0")
  initial={"0x01c20888":pd,"0x01c208ac":pe,"writes":0}
  (d/"regs").write_text(json.dumps(initial))
  env={**os.environ,"GW_UART_COMPATIBLE":str(d/"compatible"),"GW_UART_DEVMEM":str(d/"devmem"),
       "GW_FAKE_REGS":str(d/"regs"),"GW_UART_STATUS":str(d/"status")}
  if fail_write:env["GW_FAIL_WRITE"]="1"
  r=subprocess.run(["/bin/sh",str(helper),action],env=env,text=True,capture_output=True)
  assert (r.returncode==0)==(expect==0),(name,r.returncode,r.stdout,r.stderr)
  after=json.loads((d/"regs").read_text())
  if expect==0:
   assert after["0x01c20888"]==(pd&0xcfffff3f)|0x10000040,(name,after)
   assert after["0x01c208ac"]==(pe&0xfffffffc)|1,(name,after)
   assert ((after["0x01c20888"]^pd)&0xcfffff3f)==0
   assert ((after["0x01c208ac"]^pe)&0xfffffffc)==0
   if action=="apply":assert json.loads((d/"status").read_text())["ok"] is True
  if action=="check" or compatible!="custom,f1c200s-gateway" or fail_write:assert after["writes"]==0
  if pd==0x14000040 and pe==5:assert after["writes"]==0
  passed.append(name);print("PASS "+name,flush=True)
run("enable_console_and_f407_preserve_k230",0x14000000,4)
run("preserve_all_other_pin_bias_bits",0xaabbccdd,0x12345672)
run("already_correct_is_idempotent",0x14000040,5)
run("check_good_is_read_only",0x14000040,5,action="check")
run("check_bad_refuses_without_writes",0x14000000,4,action="check",expect=1)
run("wrong_board_refused",0x14000000,4,compatible="other-board",expect=1)
run("failed_write_is_not_reported_success",0x14000000,4,fail_write=True,expect=1)
print("UART_HELPER_TESTS_PASS="+str(len(passed)))

