I was able to successfully create MacOs installer packages using the
"packagemaker" command, which is scripted in "mkpkg".  Obviously, 
you do this after running make.

I haven't had a Mac for a few years, so I'm unable to re-test these
procedures.  To the best of my memory, you need to:

1.) . ./macos_env  # sets appropriate compiler env vars prior to run configure
2.) ./configure
3.) make  # insure that make using "libtool" from this directory - not Apple libtool
4.) ./mkpkg

The result of running "mkpkg" will be a directory hierarchy with top level
directory named "sqlite3odbc.pkg" which is recognized by Mac Finder as
a "package" rather then a plain directory.

This could then be copied inside a *.dmg archive file for distribution.

Chris Wolf - 28 April 2013
