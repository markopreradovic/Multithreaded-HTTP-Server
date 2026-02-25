#!/usr/bin/perl
use strict;
use warnings;
print "Content-Type: text/html\n\n";
print "<!DOCTYPE html>\n";
print "<html><head><title>CGI Test</title></head>\n";
print "<body><h1>Hello from CGI!</h1>\n";
print "<p>REQUEST_METHOD = $ENV{REQUEST_METHOD}</p>\n";
print "<p>QUERY_STRING = $ENV{QUERY_STRING}</p>\n";
print "</body></html>\n";