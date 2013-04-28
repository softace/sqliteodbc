#!/usr/bin/env python
import site, sys, os
"""
this script merges SQLite driver registration into
existing system 'odbcinst.ini'
"""
resdir = sys.argv[1] + '/Contents/Resources'
basedir = sys.argv[2]

sys.path.append(resdir + '/pylib')
site.removeduppaths()
from configobj import ConfigObj
newcfg = ConfigObj(resdir + '/odbcinst.ini')
curcfg = ConfigObj(basedir + '/Library/ODBC/odbcinst.ini')
newcfg.merge(curcfg)
newcfg.filename = basedir + '/Library/ODBC/odbcinst.ini'
newcfg.write()
