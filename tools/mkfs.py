#!/usr/bin/env python3
"""Populate a FAT32 fs.img with xv6 user programs.

Usage: mkfs.py <user_dir>
Reads fs.img in the current directory, writes all _* programs found in user_dir/.
"""

import fs
import glob
import os
import sys

user_dir = sys.argv[1] if len(sys.argv) > 1 else "user"
vfs = fs.open_fs("fat://target/fs.img")
if not vfs.exists("/bin"):
    vfs.makedir("/bin")

progs = {}
for prog in sorted(glob.glob(os.path.join(user_dir, "**/_*"), recursive=True)):
    name = os.path.basename(prog)[1:]  # strip leading _
    progs[name] = prog

for name, prog in sorted(progs.items()):
    with open(prog, "rb") as src:
        data = src.read()
    vfs.open("/" + name, "wb").write(data)
    vfs.open("/bin/" + name, "wb").write(data)

vfs.close()
