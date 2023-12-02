#!/usr/bin/python

import os
import random

try: 
  os.mkdir("/tmp/test_src_1k")
  os.mkdir("/tmp/test_dst_1k")
except:
  pass


try: 
  for j in range(8):
    os.mkdir("/tmp/test_src_1k/%d" % j)
    for i in range(1024*32):
      with open( "/tmp/test_src_1k/%d/test_%07d" % (j, i), "wb") as fi:
        sz = random.randint( 0, 4096 )
        fi.write( os.urandom(sz) )
finally: 
  pass

