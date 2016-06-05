This package also requires that you have ODBC installed and set up on
your Mac. Some GUI tools are here:

http://www.iodbc.org/dataspace/iodbc/wiki/iODBC/Downloads
http://www.odbcmanager.net

After installing the driver, which is simply just a shared library, you 
need to configure it.  Do this via the "ODBC Manager.app" located
in the /Applications/Utilites directory.  See the screen shot,
odbc_driver_config.png for more details. Then you have to configure
an entry for the database you wish to access. This can be either a
User DSN or a System DSN. I have a screen shot for a sample.

Also, you might be able to just manually edit the system file as
you would on Linux, but I haven't tested that. Some notes may be
here:  http://www.unixodbc.org/odbcinst.html

The files are at:
/Library/ODBC/odbc.ini
/Library/ODBC/odbcinst.ini

They would look something like this:

For odbc.ini:
------------------->8 cut here 8<--------------------
[ODBC Data Sources]
Mail            = SQLite3 Driver

[Mail]
Driver      = /usr/local/lib/libsqlite3odbc.dylib
Description = OSX Mail Database
database    = /Users/n9yty/Library/Mail/V3/MailData/Envelope Index
------------------->8 cut here 8<--------------------

and

For odbcinst.ini:
------------------->8 cut here 8<--------------------
[ODBC Drivers]
SQLite3 Driver        = Installed

[SQLite3 Driver]
Driver = /usr/local/lib/libsqlite3odbc.dylib
Setup  = /usr/local/lib/libsqlite3odbc.dylib
------------------->8 cut here 8<--------------------

Once configured you can use the iodbctest or iodbctestw (unicode)
commands in Terminal to verify you can make a connection.

