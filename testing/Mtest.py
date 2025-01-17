#!/usr/bin/python2

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0.  If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Copyright 2008-2015 MonetDB B.V.

#TODO:
#=====
# - check all TODO's below
# - tidy -up HTML-generation by "keeping in mind" during testing,
#   which OUT/ERR differ or not and which tests were skipped.
#   dump HTML-stuff only at end
#   print an ascii summary at end, too
# - if no diffs, but warnings, say so at end
# - produce, keep & reference LOG
# - add a "grep-like" function and replace "inlined" grep
#   contains(<file>,<string>)
# - do multi-level prompting?
# - normalize all path's used
# - Python 3? (or do a full rewrite?)

import os
import sys
import shutil
import re
import random
import time
import socket
import struct
import signal
import Mfilter
import fnmatch
import glob

procdebug = False
verbose = False
quiet = False

releaserun = False

# whether output goes to a tty
isatty = os.isatty(sys.stdout.fileno())

if os.name != 'nt' and isatty:
    # color output a little
    RED = '\033[1;31m'
    GREEN = '\033[0;32m'
    PURPLE = '\033[1;35m'       # actually magenta
    BLACK = '\033[0;0m'
else:
    # no coloring if not to a tty
    RED = GREEN = PURPLE = BLACK = ''

def ErrExit(msg):
    print >> sys.stderr, msg
    sys.exit(1)

def _configure(str):
    # expand configure variables in str and return result
    config = [
        ('{source}', '/home/release/release/MonetDB'),
        ('${build}', '/home/release/release/MonetDB'),

        ('${bindir}', '${exec_prefix}/bin'),
##        ('${sbindir}', '@QXsbindir@'),
        ('${libexecdir}', '${exec_prefix}/libexec'),
        ('${datarootdir}', '${prefix}/share'),
        ('${datadir}', '${prefix}/share'),
        ('${sysconfdir}', '${prefix}/etc'),
##        ('${sharedstatedir}', '@QXsharedstatedir@'),
        ('${localstatedir}', '${prefix}/var'),
        ('${libdir}', '${exec_prefix}/lib'),
        ('${includedir}', '${prefix}/include'),
##        ('${oldincludedir}', '@QXoldincludedir@'),
        ('${infodir}', '${datarootdir}/info'),
        ('${mandir}', '${datarootdir}/man'),
        ('${Qbindir}', '${exec_prefix}/bin'),
##        ('${Qsbindir}', '@QXsbindir@'),
        ('${Qlibexecdir}', '${exec_prefix}/libexec'),
        ('${Qdatarootdir}', '${prefix}/share'),
        ('${Qdatadir}', '${prefix}/share'),
        ('${Qsysconfdir}', '${prefix}/etc'),
##        ('${Qsharedstatedir}', '@QXsharedstatedir@'),
        ('${Qlocalstatedir}', '${prefix}/var'),
        ('${Qlibdir}', '${exec_prefix}/lib'),
        ('${Qincludedir}', '${prefix}/include'),
##        ('${Qoldincludedir}', '@QXoldincludedir@'),
        ('${Qinfodir}', '${datarootdir}/info'),
        ('${Qmandir}', '${datarootdir}/man'),
        # put these at end (in this order!) for efficiency
        ('${exec_prefix}', '${prefix}'),
        ('${Qexec_prefix}', '${prefix}'),
        ('${prefix}', '/usr/local'),
        ('${Qprefix}', '/usr/local'),
        ]
    if os.name == 'nt':
        str = str.replace('%prefix%', '${prefix}')
        str = str.replace('%exec_prefix%', '${exec_prefix}')
    changed = True
    while '$' in str and changed:
        changed = False
        for key, val in config:
            if os.name == 'nt':
                val = val.replace('%prefix%', '${prefix}')
                val = val.replace('%exec_prefix%', '${exec_prefix}')
            nstr = str.replace(key, val)
            changed = changed or str != nstr
            str = nstr
    return str

# use our own process module because it has _BufferedPipe
try:
    import process
except ImportError:
    try:
        import MonetDBtesting.process as process
    except ImportError:
        p = _configure(os.path.join('/usr/local', 'lib/python2.7/site-packages'))
        sys.path.insert(0, p)
        import MonetDBtesting.process as process
        if os.environ.has_key('PYTHONPATH'):
            p += os.pathsep + os.environ['PYTHONPATH']
        os.environ['PYTHONPATH'] = p

# Replace os.fork by a version that forks but also sets the process
# group in the child.  This is done so that we can easily kill a
# subprocess and its children in case of a timeout.
# To use this, set the global variable setpgrp to True before calling
# subprocess.Popen.  It is reset automatically to False so that
# subprocess of our child don't get their own process group.
try:
    os.setpgrp
except AttributeError:
    try:
        os.setpgid
    except AttributeError:
        # no function to set process group, so don't replace
        pass
    else:
        # use os.setpgid to set process group
        def myfork(osfork = os.fork):
            global setpgrp
            _setpgrp = setpgrp
            setpgrp = False
            pid = osfork()
            if pid == 0 and _setpgrp:
                os.setpgrp()
            return pid
        os.fork = myfork
else:
    # use os.setpgrp to set process group
    def myfork(osfork = os.fork):
        global setpgrp
        _setpgrp = setpgrp
        setpgrp = False
        pid = osfork()
        if pid == 0 and _setpgrp:
            os.setpgid(0, 0)
        return pid
    os.fork = myfork
setpgrp = False

ttywidth = 0
if isatty and os.isatty(sys.stdin.fileno()):
    if os.name == 'nt':
        ttywidth = 79
    else:
        try:
            proc = process.Popen(['stty', '-a'], stdout = process.PIPE,
                                 stderr = process.PIPE)
        except OSError:
            pass
        else:
            out, err = proc.communicate()
            res = re.search('columns ([0-9]+)', out)
            if res is not None:
                ttywidth = int(res.group(1))
            else:
                res = re.search(' ([0-9]+) columns', out)
                if res is not None:
                    ttywidth = int(res.group(1))
            if ttywidth < 60:
                # rediculously narrow tty, ignore value
                ttywidth = 0

import string                   # for whitespace
def splitcommand(cmd):
    '''Like string.split, except take quotes into account.'''
    q = None
    w = []
    command = []
    for c in cmd:
        if q:
            if c == q:
                q = None
            else:
                w.append(c)
        elif c in string.whitespace:
            if w:
                command.append(''.join(w))
            w = []
        elif c == '"' or c == "'":
            q = c
        else:
            w.append(c)
    if w:
        command.append(''.join(w))
    if len(command) > 1 and command[0] == 'call':
        del command[0]
    return command

def remove(file):
    try:
        os.remove(file)
    except:
        pass

def isexecutable(TST, ext = '.sh') :
    if   os.name == "nt":
        for ext in ".exe", ".com", ".bat", ".cmd":
            if TST.lower().endswith(ext):
                ext = ''
            if os.path.isfile(TST+ext) or os.path.isfile(TST+ext+".src"):
                return [ 1, ext ]
    elif os.name == "posix":
        #TODO:
        # check with "file", and set executable
        TST += ext
        if ( os.path.isfile(TST       ) and os.access(TST       ,os.X_OK) ) or \
           ( os.path.isfile(TST+".src") and os.access(TST+".src",os.X_OK) ):
            return [ 1, ext ]
    #TODO:
    #else:
        # ???
    return [ 0, "" ]
### isexecutable(TST, ext = '.sh') #

def CheckExec(cmd) :
    for p in os.environ['PATH'].split(os.pathsep):
        x = isexecutable(os.path.join(p,cmd),'')
        if x[0]:
            return os.path.join(p, cmd + x[1])
    return ""
### CheckExec(cmd) #

try:
    import monet_options
except ImportError:
    try:
        from MonetDBtesting import monet_options
    except ImportError:
        p = _configure(os.path.join('/usr/local', 'lib/python2.7/site-packages'))
        sys.path.insert(0, p)
        from MonetDBtesting import monet_options
        if os.environ.has_key('PYTHONPATH'):
            p += os.pathsep + os.environ['PYTHONPATH']
        os.environ['PYTHONPATH'] = p

import threading

randomPortRepeat = 9

F_SKIP = -1
F_OK = 0
F_WARN = 1
F_SOCK = 2
F_ERROR = 3
F_TIME = 4
F_ABRT = 5
F_RECU = 6
F_SEGV = 7

FAILURES = {
    F_SKIP  : ("F_SKIP",  '-'),
    F_OK    : ("F_OK",    'o'),
    F_WARN  : ("F_WARN",  'x'),
    F_SOCK  : ("F_SOCK",  'S'),
    F_ERROR : ("F_ERROR", 'X'),
    F_TIME  : ("F_TIME",  'T'),
    F_ABRT  : ("F_ABRT",  'A'),
    F_RECU  : ("F_RECU",  'R'),
    F_SEGV  : ("F_SEGV",  'C'),
}

CONDITIONALS = {
    # X == true   =>  @X_TRUE@='',  @X_FALSE@='#'
    # X == false  =>  @X_TRUE@='#', @X_FALSE@=''
    # from configure.ag:
    # These should cover all AM_CONDITIONALS defined in configure.ag, i.e.,
    # `grep AM_CONDITIONAL configure.ag | sed 's|^AM_CONDITIONAL(\([^,]*\),.*$|\1|' | sort -u`
    'BITS32'               : "",
    'BITS64'               : "#",
    'BITS64OID32'          : "",
    'CROSS_COMPILING'      : "",
    'HAVE_CURL'            : "#",
    'HAVE_DEVELOPER'       : "#",
    'HAVE_FITS'            : "",
    'HAVE_GDK'             : "#",
    'HAVE_GEOM'            : "#",
    'HAVE_GSL'             : "",
    'HAVE_HGE'             : "#",
    'HAVE_JAVA'            : "",
    'HAVE_JAVAJDBC'        : "",
    'HAVE_JAVAMEROCONTROL' : "",
    'HAVE_LIBBZ2'          : "#",
    'HAVE_LIBR'            : "",
    'HAVE_LIBZ'            : "#",
    'HAVE_MONETDB5'        : "#",
    'HAVE_NETCDF'          : "",
    'HAVE_ODBC'            : "#",
    'HAVE_PCRE'            : "#",
    'HAVE_PERL'            : "#",
    'HAVE_PYTHON'          : "#",
    'HAVE_PYTHON2'         : "#",
    'HAVE_PYTHON3'         : "#",
    'HAVE_RUBYGEM'         : "#",
    'HAVE_SAMTOOLS'        : "",
    'HAVE_SPHINXCLIENT'    : "",
    'HAVE_SQL'             : "#",
    'HAVE_TESTING'         : "#",
    'NATIVE_WIN32'         : "",
    'NOT_WIN32'            : "#",
    'PROFILING'            : "",
    # unknown at compile time;
    # hence, we set them only at runtime in main() below
    'HAVE_MONETDBJDBC_JAR' : "",
    'HAVE_JDBCCLIENT_JAR'  : "",
    'HAVE_JDBCTESTS_JAR'   : "",
    'HAVE_JDBCTESTS_DIR'   : "",
    'HAVE_JDBCTESTS'       : "",
    'MERCURIAL'            : "",
}

# a bunch of classes to help with generating (X)HTML files
class _Encode:
    # mix-in class for encoding text and attribute values so that they
    # don't get interpreted as something else by the browser
    def encode(self, data, attr):
        map = [('&', '&amp;'),          # MUST be first
               ('<', '&lt;'),
               ('>', '&gt;'),
               (None, None),
               # following chars only translated in attr values (attr is True)
               ('"', '&quot;'),
               ('\t', '&#9;'),
               ('\n', '&#10;'),
               ('\r', '&#13;'),
               ]
        for c, tr in map:
            if c is None:
                if not attr:
                    break
                continue
            data = data.replace(c, tr)
        return data

class Element(_Encode):
    # class to represent an (X)HTML element with its attributes and
    # children

    # inline elements, we do not add newlines to the contents of these
    # elements
    inline = ['tt','i','b','big','small','em','strong','dfn','code',
              'samp','kbd','var','cite','abbr','acronym','a','img',
              'object','br','script','map','q','sub','sup','span',
              'bdo','input','select','textarea','label','button','font']
    # empty elements
    empty = ['link', 'basefont', 'br', 'area', 'img', 'param', 'hr',
             'input', 'col', 'frame', 'isindex', 'base', 'meta', ]
    xml = True                          # write XHTML instead of HTML

    def __init__(self, tag, attrdict = None, *children):
        self.tag = tag
        if attrdict is None:
            attrdict = {}
        self.attrdict = attrdict
        if children is None:
            children = []
        self.isempty = tag.lower() in self.empty
        if self.isempty:
            if children:
                raise ValueError("empty element can't have children")
            self.children = None
        else:
            self.children = list(children)

    def __str__(self):
        # string representation of the element with its children
        s = ['<%s' % self.tag]
        attrlist = self.attrdict.items()
        attrlist.sort()
        for name, value in attrlist:
            s.append(' %s="%s"' % (name, self.encode(value, True)))
        if self.children or (not self.xml and not self.isempty):
            s.append('>')
            for c in self.children:
                s.append(str(c))
            s.append('</%s>' % self.tag)
        elif self.xml:
            s.append('/>')
        else:
            s.append('>')               # empty HTML element
        return ''.join(s)

    def write(self, f, newline = False):
        # write the element with its children to a file
        # if newline is set, add newlines at strategic points
        if self.tag.lower() == 'html':
            # before we write the DOCTYPE we should really check
            # whether the document conforms...
            if self.xml:
                f.write('<!DOCTYPE html PUBLIC '
                        '"-//W3C//DTD XHTML 1.0 Transitional//EN"\n'
                        '                      '
                        '"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">\n')
            else:
                f.write('<!DOCTYPE html PUBLIC '
                        '"-//W3C//DTD HTML 4.01 Transitional//EN"\n'
                        '                      '
                        '"http://www.w3.org/TR/html4/loose.dtd">\n')
        inline = self.tag.lower() in self.inline
        f.write('<%s' % self.tag)
        attrlist = self.attrdict.items()
        attrlist.sort()
        for name, value in attrlist:
            f.write(' %s="%s"' % (name, self.encode(value, True)))
        if self.children or (not self.xml and not self.isempty):
            if not inline:
                for c in self.children:
                    if not isinstance(c, Element):
                        inline = True
                        break
            f.write('>')
            if newline and not inline:
                f.write('\n')
            for c in self.children:
                c.write(f, newline and not inline)
            f.write('</%s>' % self.tag)
        elif self.xml:
            f.write('/>')
        else:
            f.write('>')                # empty HTML element
        if newline:
            f.write('\n')

    def addchild(self, child):
        self.children.append(child)

    def addchildren(self, children):
        for child in children:
            self.children.append(child)

    def inschild(self, index, child):
        self.children.insert(index, child)

class Text(_Encode):
    # class to represent text in (X)HTML
    def __init__(self, text = '', raw = False):
        self.text = text
        self.raw = raw

    def __str__(self):
        if self.raw:
            return self.text
        return self.encode(self.text, False)

    def write(self, f, newline = False):
        f.write(str(self))
        if newline and not self.raw:
            f.write('\n')

class Comment:
    # class to represent an (X)HTML comment (not currently used)
    def __init__(self, text):
        self.text = text

    def __str__(self):
        return '<!--%s-->' % self.text

    def write(self, f, newline = False):
        f.write(str(self))

class Timer:
    # interface to the threading.Timer function that interprets a
    # timeout of 0 as no timeout
    def __init__(self, interval, function, args):
        self.timer = None
        if interval > 0:
            self.timer = threading.Timer(interval, function, args = args)

    def start(self):
        if self.timer is not None:
            self.timer.start()

    def cancel(self):
        if self.timer is not None:
            self.timer.cancel()

STDOUT = sys.stdout
STDERR = sys.stdout     # err
REV = ''                # revision (output of hg id), default unknown

black = 'black'                         # #000000
white = 'white'                         # #ffffff
red = 'red'                             # #ff0000
lime = 'lime'                           # #00ff00
green = '#00aa00'
darkgreen = '#005500'
orange = '#ffaa00'
purple = '#aa00aa'
stylesheet = Element('style', None, Text('''
.error     { font-weight: bold; font-style: italic; color: red; }
.segfault  { font-weight: bold; font-style: italic; color: purple; }
.abort     { font-weight: bold; font-style: italic; color: purple; }
.recursion { font-weight: bold; font-style: italic; color: purple; }
.timeout   { font-weight: bold; font-style: italic; color: purple; }
.socket    { font-weight: bold; font-style: italic; color: purple; }
.warning   { font-weight: bold; color: orange; }
.good      {  }
.header    { font-family: helvetica, arial; text-align: center; }
.black     { color: black; }
'''))

TIMES = []

random.seed(time.time())

def Usage (options) :
    try:
        monet_options.usage(options, '%s [options] ( [<dir>] [<tests>] | [<dirs>] )' % THISFILE)
    except monet_options.Error:
        pass

    sys.stderr.write("""
 <dir>   : if present, %(prog)s behaves as if called in <dir>
 <tests> : list of tests to be processed; if none or 'All' is given,
            all tests listed in 'Tests/All' are processed
            (defaults to 'All' if -r is used)
 <dirs>  : list of directories to be processed; if present, %(prog)s
            processes 'All' tests in each directory of <dirs>; -r may be used also

         See  %(readme)s
         for details about  %(prog)s.
""" % {'prog': THISFILE,
       'readme': os.path.join('/home/release/release/MonetDB','testing','README'),
       })
    raise

### Usage () #

#TODO:
#class TimeoutError:
#       def __init__(self, text):
#               self.text = text
#       def __str__(self):
#               return self.text
#
#def AlarmHandler(signum, frame) :
#       raise TimeoutError, "Timeout"
#### AlarmHandler(signum, frame) #

def ErrMsg(TEXT) :
    STDOUT.flush()
    STDERR.write("\n%s:  ERROR:  %s\n\n" % (THISFILE, TEXT))
    STDERR.flush()
### ErrMsg(TEXT) #

def ErrXit(TEXT) :
    ErrMsg(TEXT)
    sys.exit(1)
### ErrXit(TEXT) #

def Warn(TEXT) :
    try:
        STDOUT.flush()
    except IOError:
        pass
    try:
        STDERR.write("\n%s  Warning:  %s\n\n" % (THISFILE, TEXT))
        STDERR.flush()
    except IOError:
        pass
### Warn(TEXT) #

def startswithpath(str,pre) :
    return os.path.normcase(str[:len(pre)]) == os.path.normcase(pre)
### startswithpath(str,pre) #

import urllib
##def path(str) :
##    return str.replace('/', os.sep)
path = urllib.url2pathname
### path(str) #

##def url(str) :
##    return str.replace(os.sep, '/')
url = urllib.pathname2url
### url(str) #

def try_open(path, mode) :
    try:
        f = open(path, mode)
    except IOError, (IOerrNo, IOerrStr):
        Warn("Opening file '%s' in mode '%s' failed with #%d: '%s'." % (path, mode, IOerrNo, IOerrStr))
        f = None
    return f
###  try_open(path, mode) #

def CreateHtmlIndex (env, *body) :
    TSTDIR=env['TSTDIR']
    TSTTRGDIR=env['TSTTRGDIR']

    if TSTDIR:
        INDEX=".index"
    else:
        INDEX="index"

    if body:
        BACK = os.getcwd()
        os.chdir(TSTTRGDIR)

        if TSTDIR:
            header = Text(TSTDIR)
            if URLPREFIX:
                header = Element('a',
                                 {'href': '%s%s/%s' % (URLPREFIX, url(TSTDIR), TSTSUFF),
                                  'target': '%s_%s_body' % (DISTVER, TSTDIR),
                                  'class': 'black'},
                                 header)
        else:
            header = Element('span', {'class': 'black'},
                             Text(DISTVER))
        tr = Element('tr', {'valign': 'top'},
                     Element('th', {'class': 'header'},
                             header))
        tr.addchildren(body)
        html = Element('html', {},
                       Element('head', {},
                               Element('title', {},
                                       Text(HTMLTITLE)),
                               stylesheet),
                       Element('body',
                               {'bgcolor': white,
                                'text': black,
                                'link': green,
                                'vlink': darkgreen,
                                'alink': lime},
                               Element('center', {},
                                       Element('table',
                                               {'align': 'abscenter',
                                                'border': '1',
                                                'cellspacing': '0',
                                                'cellpadding': '3'},
                                               tr))))
        f = open("%s.head.html" % INDEX,"w")
        html.write(f, True)
        f.close()

        if TSTDIR:
            ROWS="72"
        else:
            ROWS="54"
        html = Element('html', {},
                       Element('head', {},
                               Element('title', {}, Text(HTMLTITLE))),
                       Element('frameset',
                               {'rows': '%s,*' % ROWS,
                                'frameborder': 'yes',
                                'border': '1',
                                'bordercolor': white,
                                'marginwidth': '0',
                                'marginheight': '0'},
                               Element('frame',
                                       {'src': '%s.head.html' % INDEX,
                                        'scrolling': 'auto',
                                        'name': '%s_%s_head' % (DISTVER, TSTDIR),
                                        'frameborder': 'yes',
                                        'bordercolor': white,
                                        'marginwidth': '0',
                                        'marginheight': '0'}),
                               Element('frame',
                                       {'src': url(env['_%s_BODY_' % TSTDIR][0]),
                                        'scrolling': 'auto',
                                        'name': '%s_%s_body' % (DISTVER, TSTDIR),
                                        'frameborder': 'yes',
                                        'bordercolor': white,
                                        'marginwidth': '0',
                                        'marginheight': '0'})))
        f = open("%s.html" % INDEX, "w")
        html.write(f, True)
        f.close()
        env['_%s_BODY_' % TSTDIR] = ["", 0]
        os.chdir(BACK)
### CreateHtmlIndex (env, *body) #

bugre = re.compile(r'\.(sf|bug)-(?P<bugno>[1-9]\d+)', re.I)

def CreateTstWhatXhtml (env, TST, stableWHAT, EXT, result) :
    WHAT = stableWHAT[7:11]
    TSTDIR    = env['TSTDIR']
    TSTSRCDIR = env['TSTSRCDIR']

    if result == F_OK:
        diffclass = 'good'
        difftext = 'No differences'
    elif result == F_WARN:
        diffclass = 'warning'
        difftext = 'Minor differences'
    else:                       # result == F_ERROR:
        difftext = 'Major differences'
        if result == F_SOCK:
            diffclass = 'socket'
            difftext += ' (Socket)'
        elif result == F_TIME:
            diffclass = 'timeout'
            difftext += ' (Timeout)'
        elif result == F_RECU:
            diffclass = 'recursion'
            difftext += ' (Recursion)'
        elif result == F_ABRT:
            diffclass = 'abort'
            difftext += ' (Aborted)'
        elif result == F_SEGV:
            diffclass = 'segfault'
            difftext += ' (Crash)'
        else:
            diffclass = 'error'

    if COMPBITSOIDSLINK:
        SYSTEM = '%s, %s:' % (DISTVER, COMPBITSOIDSLINK)
    else:
        SYSTEM = "%s:" % DISTVER

    html = Element('html', {},
                   Element('head', {},
                           Element('title', {}, Text(HTMLTITLE)),
                           stylesheet),
                   Element('frameset', {'rows': '42,*',
                                        'frameborder': 'yes',
                                        'border': '1',
                                        'bordercolor': white,
                                        'marginwidth': '0',
                                        'marginheight': '0'},
                           Element('frame',
                                   {'src': '.%s%s.head.html' % (TST, WHAT),
                                    'scrolling': 'auto',
                                    'name': '%s_%s_%s_%s_head' % (DISTVER, TSTDIR, TST, WHAT[1:]),
                                    'frameborder': 'yes',
                                    'bordercolor': white,
                                    'marginwidth': '0',
                                    'marginheight': '0'}),
                           Element('frame',
                                   {'src': '%s%s.diff.html' % (TST, WHAT),
                                    'scrolling': 'auto',
                                    'name': '%s_%s_%s_%s_body' % (DISTVER, TSTDIR, TST, WHAT[1:]),
                                    'frameborder': 'yes',
                                    'bordercolor': white,
                                    'marginwidth': '0',
                                    'marginheight': '0'})))
    f = open(".%s%s.html" % (TST, WHAT), "w")
    html.write(f, True)
    f.close()
    f = open(".%s%s.head.html" % (TST, WHAT),"w")
    target = '%s_%s_%s_%s_body' % (DISTVER, TSTDIR, TST, WHAT[1:])
    if REV:                     # implies URLPREFIX is not None
        urlpref = '%s%s/%s' % (URLPREFIX, url(TSTDIR), TSTSUFF)
        hg = Element('a', {'href': urlpref,
                           'target': target},
                     Text('hg'))
    else:
        hg = None
    text = Element('div', {'class': 'header'},
                   Text(SYSTEM),
                   Text(' '),
                   Element('a', {'href': '%s%s.diff.html' % (TST, WHAT),
                                 'target': target,
                                 'class': diffclass},
                           Text(difftext)),
                   Text(' between '),
                   Element('a', {'href': '%s%s' % (TST, stableWHAT),
                                 'target': target},
                           Text(stableWHAT[1:])))
    if REV:
        d = urlpref
        if os.path.isfile(TST + stableWHAT + '.src'):
            # there's only one file like this...
            fl = open(TST + stableWHAT + '.src').readline().strip()
            if fl.startswith('$RELSRCDIR/'):
                fl = fl[11:]
                while fl.startswith('../'):
                    fl = fl[3:]
                    d = d[:d.rindex('/')]
            fl = '%s/%s' % (d, fl)
        else:
            fl = '%s/%s%s' % (d, TST, stableWHAT)
        text.addchildren([
                Text(' (id '),
                Element('a', {'href': fl,
                              'target': target}, Text(REV)),
                Text(')')])
    text.addchildren([
            Text(' and '),
            Element('a', {'href': '%s.test%s' % (TST, WHAT),
                          'target': target},
                    Text('test%s' % WHAT)),
            Text(' of '),
            Element('a', {'href': TST + EXT, 'target': target},
                    Text(TST + EXT))])
    if REV:
        d = urlpref
        if os.path.isfile(TST + EXT + '.src'):
            fl = open(TST + EXT + '.src').readline().strip()
            if fl.startswith('$RELSRCDIR/'):
                fl = fl[11:]
                while fl.startswith('../'):
                    fl = fl[3:]
                    d = d[:d.rindex('/')]
            fl = '%s/%s' % (d, fl)
        elif os.path.isfile(TST + EXT + '.in'):
            fl = '%s/%s%s.in' % (d, TST, EXT)
        else:
            fl = '%s/%s%s' % (d, TST, EXT)
        text.addchildren([
                Text(' (id '),
                Element('a', {'href': fl,
                              'target': target}, Text(REV)),
                Text(')')])
    text.addchildren([
            Text(' in '),
            Element('a', {'href': './', 'target': target},
                    Text(TSTDIR)),
            Text(' (')])
    if hg:
        text.addchild(hg)
        text.addchild(Text(', '))
    text.addchildren([
            Element('a', {'href': url(env['RELSRCDIR']),
                          'target': target},
                    Text('src')),
            Text(')')])
    res = bugre.search(TST)
    if res is not None:
        bugno = res.group('bugno')
        text.addchildren([
                Text(' ('),
                Element('a', {'href': 'http://bugs.monetdb.org/%s' % bugno,
                              'target': '_blank'},
                        Text('Bugzilla')),
                Text(')')])
    html = Element('html', {},
                   Element('head', {},
                           Element('title', {},
                                   Text(HTMLTITLE)),
                           stylesheet),
                   Element('body',
                           {'bgcolor': white,
                            'text': black,
                            'link': green,
                            'vlink': darkgreen,
                            'alink': lime},
                           text))

    html.write(f, True)
    f.close()
#TODO?
# <A HREF='.Mtest.Slave.Log.OutErr' TARGET='"""+DISTVER+"_"+TSTDIR+"_"+TST+"_"+WHAT[1:]+"""_body'>LOG</A>).
### CreateTstWhatXhtml (env, TST, stableWHAT, EXT, results) #

def CreateSrcIndex (env, TST, EXT) :
    TSTSRCDIR = env['TSTSRCDIR']
    TSTDIR    = env['TSTDIR']

    if URLPREFIX:
        framesrc = '%s%s/%s/%s%s' % (URLPREFIX, url(TSTDIR), TSTSUFF, TST, EXT)
    else:
        f = open(".%s.nosrc.index.html" % TST, "w")
        html = Element('html', {},
                       Element('head', {},
                               Element('title', {},
                                       Text(HTMLTITLE)),
                               stylesheet),
                       Element('body',
                               {'bgcolor': white,
                                'text': black,
                                'link': green,
                                'vlink': darkgreen,
                                'alink': lime},
                               Element('center', {},
                                       Text('no source available'))))
        framesrc = '.%s.nosrc.index.html' % TST
    html = Element('html', {},
                   Element('head', {},
                           Element('title', {}, Text(HTMLTITLE))),
                   Element('frameset',
                           {'rows': '54,*',
                            'frameborder': 'yes',
                            'border': '1',
                            'bordercolor': white,
                            'marginwidth': '0',
                            'marginheight': '0'},
                           Element('frame',
                                   {'src': '.%s.src.index.head.html' % TST,
                                    'scrolling': 'auto',
                                    'name': '%s_%s_%s_head' % (DISTVER, TSTDIR, TST),
                                    'frameborder': 'yes',
                                    'bordercolor': white,
                                    'marginwidth': '0',
                                    'marginheight': '0'}),
                           Element('frame',
                                   {'src': framesrc,
                                    'scrolling': 'auto',
                                    'name': '%s_%s_%s_body' % (DISTVER, TSTDIR, TST),
                                    'frameborder': 'yes',
                                    'bordercolor': white,
                                    'marginwidth': '0',
                                    'marginheight': '0'})))
    f = open(".%s.src.index.html" % TST,"w")
    html.write(f, True)
    f.close()

    tr = Element('tr', {},
                 Element('th', {'class': 'header'},
                         Text(TST)))
    for s in os.listdir(TSTSRCDIR):
        if s.startswith(TST):
            slink = Text(s)
            if URLPREFIX:
                slink = Element('a',
                                {'href': '%s%s/%s/%s' % (URLPREFIX, url(TSTDIR), TSTSUFF, s),
                                 'target': '%s_%s_%s_body' % (DISTVER, TSTDIR, TST)},
                                slink)
            tr.addchild(Element('td', {'class': 'header'},
                                slink))
    html = Element('html', {},
                   Element('head', {},
                           Element('title', {},
                                   Text(HTMLTITLE)),
                           stylesheet),
                   Element('body',
                           {'bgcolor': white,
                            'text': black,
                            'link': green,
                            'vlink': darkgreen,
                            'alink': lime},
                           Element('center', {},
                                   Element('table',
                                           {'align': 'abscenter',
                                            'border': '1',
                                            'cellspacing': '0',
                                            'cellpadding': '3'},
                                           tr))))
    f = open(".%s.src.index.head.html" % TST, "w")
    html.write(f, True)
    f.close()
### CreateSrcIndex (env, TST, EXT) #

def AddHref (href, target, linktext, diff) :
    if   diff == F_ERROR:
        klass = 'error'
    elif diff == F_RECU:
        klass = 'recursion'
    elif diff == F_TIME:
        klass = 'timeout'
    elif diff == F_SOCK:
        klass = 'socket'
    elif diff == F_ABRT:
        klass = 'abort'
    elif diff == F_SEGV:
        klass = 'segfault'
    elif diff == F_WARN:
        klass = 'warning'
    else:
        klass = 'good'
    a = Element('a', {'href': href, 'target': target, 'class': klass},
                Text(linktext))
    if klass == 'good':
        return [Text('('), a, Text(')')]
    else:
        return [a]
### AddHref (TSTDIR, TST, WHAT, diff) #

def AddTstToHtmlIndex (env, TST, STABLEout, STABLEerr, EXT, o, e) :
    TSTDIR = env['TSTDIR']

    CreateTstWhatXhtml(env, TST, STABLEout, EXT, o)
    CreateTstWhatXhtml(env, TST, STABLEerr, EXT, e)

    if o == F_ERROR or e == F_ERROR:
        tstclass = 'error'
    elif e == F_RECU:
        tstclass = 'recursion'
    elif e == F_TIME:
        tstclass = 'timeout'
    elif e == F_SOCK:
        tstclass = 'socket'
    elif e == F_ABRT:
        tstclass = 'abort'
    elif e == F_SEGV:
        tstclass = 'segfault'
    elif o == F_WARN or e == F_WARN:
        tstclass = 'warning'
    else:
        tstclass = 'good'

    td = Element('td', {'class': 'header'},
                 Element('a', {'href': '.%s.src.index.html' % TST,
                               'target': '%s_%s_body' % (DISTVER, TSTDIR),
                               'class': tstclass},
                         Text(TST)),
                 Element('br'))
    td.addchildren(AddHref('.%s%s.html' % (TST, '.out'),
                           '%s_%s_body' % (DISTVER, TSTDIR),
                           'out', o))
    td.addchild(Text("&nbsp;|&nbsp;", raw = True))
    td.addchildren(AddHref('.%s%s.html' % (TST, '.err'),
                           '%s_%s_body' % (DISTVER, TSTDIR),
                           'err', e))
    if not env.has_key('_%s_BODY_' % TSTDIR)  or  \
       not env['_%s_BODY_' % TSTDIR][0]  or  \
       ( (not env['_%s_BODY_' % TSTDIR][1])  and  (o or e) ):
        if e and not o:
            env['_%s_BODY_' % TSTDIR] = [".%s.err.html" % TST, e]
        else:
            env['_%s_BODY_' % TSTDIR] = [".%s.out.html" % TST, o]

    CreateSrcIndex(env, TST, EXT)

    return td
### AddTstToHtmlIndex (env, TST, STABLEout, STABLEerr, EXT) #

def AddSubToHtmlIndex (env, TSTDIR, diff) :
    td = Element('td', {'class': 'header'})
    td.addchildren(AddHref('%s/.index.html' % url(TSTDIR), '%s__body' % DISTVER,
                           TSTDIR, diff))
    if not env.has_key('__BODY_')  or  \
       not env['__BODY_'][0]  or  \
       ( (not env['__BODY_'][1])  and  diff ):
        env['__BODY_'] = ["%s/.index.html" % TSTDIR, diff]
    return td
### AddSubToHtmlIndex (env, TSTDIR, diff) #

def SkipTest(env, TST, EXT, REASON, length) :
    TSTDIR = env['TSTDIR']
    TEXT = "Skipping test %s%s %s" % (TST, EXT, REASON)
    if quiet:
        STDOUT.write("-")
    elif verbose:
        Warn(TEXT)
    else:
        if REASON.startswith('as '):
            REASON = REASON[3:]
        if REASON.endswith('.'):
            REASON = REASON[:-1]
        if length + 10 + len(REASON) + 11 > ttywidth:
            # 10 - length of prompt()
            # 11 - length of " skipped ()"
            l = ttywidth - 10 - 11 - len(REASON)
            if len(TST) <= l:
                s = '%-*s' % (l, TST)
            else:
                s = '%s...%s' % (TST[:l/2 - 2], TST[-(l/2 - 1):])
        else:
            s = '%-*s' % (length, TST)
        STDOUT.write('%s%s skipped (%s)\n' % (prompt(), s, REASON))

    if testweb:
        return None

    f = open(".%s.SKIPPED" % TST, "w")
    f.write("\n%s  Warning:  %s\n\n" % (THISFILE, TEXT))
    f.close()
    target = '%s_%s_body' % (DISTVER, TSTDIR)
    td = Element('td', {'class': 'header'},
                 Element('a', {'href': '.%s.src.index.html' % TST,
                               'target': target,
                               'class': 'black'},
                         Text(TST)),
                 Element('br'),
                 Element('a', {'href': '.%s.SKIPPED' % TST,
                               'target': target},
                         Text('(skipped)')))
    if not env.has_key('_%s_BODY_' % TSTDIR)  or  \
       not env['_%s_BODY_' % TSTDIR][0]  or  \
       not env['_%s_BODY_' % TSTDIR][1]:
        env['_%s_BODY_' % TSTDIR] = [".%s.SKIPPED" % TST, F_SKIP]
    CreateSrcIndex(env, TST, EXT)
    return td
### SkipTest(env, TST, EXT, REASON) #

def find_test_dirs(thisdir) :
    testdirs = []
    thisdir = os.path.realpath(thisdir)
    dirnme = os.path.basename(thisdir)
    dirlst = os.listdir(thisdir)
    if dirnme == TSTSUFF  and  "All" in dirlst  and  os.path.isfile(os.path.join(thisdir,"All")):
        testdirs.append(os.path.dirname(thisdir))
    for d in dirlst:
        d = os.path.join(thisdir,d)
        if os.path.isdir(d):
            testdirs = testdirs + find_test_dirs(d)
    return testdirs
### find_test_dirs(thisdir) #

def PerformDir(env, testdir, testlist, BusyPorts) :
    td = 0
    elem = None
    FdOut = F_SKIP
    FdErr = F_SKIP
    if testdir == TSTSRCBASE:
        TSTDIR = os.curdir
    else:
        TSTDIR = testdir[len(TSTSRCBASE + os.sep):]
    TSTSRCDIR = os.path.normpath(os.path.join(testdir, TSTSUFF))
    TSTTRGDIR = os.path.normpath(os.path.join(TSTTRGBASE, TSTPREF, TSTDIR))

    if THISFILE == "Mtest.py":
        TSTDB = TSTPREF + "_" + TSTDIR.replace(os.sep, '_')
    else: # THISFILE == "Mapprove.py"
        TSTDB = ""

    if testlist:
        tl = []
        for tst in testlist:
            tl.append((tst,None))
        testlist = tl
    else:
        for tc in open(os.path.join(TSTSRCDIR,"All")):
            if tc.find('#') >= 0:
                # get rid of comment anywhere on line
                tc = tc.split('#', 1)[0]
            tc = tc.strip()
            if tc:
                if tc.find('?') > -1:
                    cond,tst = tc.split('?')
                else:
                    cond,tst = None,tc
                testlist.append((tst,cond))
    if not testlist:
        Warn("No tests found in '%s`; skipping directory!" % TSTSRCDIR)
        return td, elem, max(FdOut, FdErr)

    # find length of longest test name
    length = 0
    for tst in testlist:
        if len(tst[0]) > length:
            length = len(tst[0])

    env['TSTDB']     = TSTDB
    env['TSTDIR']    = TSTDIR
    env['TSTSRCDIR'] = TSTSRCDIR
    env['UTSTSRCDIR'] = 'file:' + url(TSTSRCDIR)
    env['TSTTRGDIR'] = TSTTRGDIR
    if TSTDIR == os.curdir:
        env['RELSRCDIR'] = os.path.join(os.pardir, env['RELSRCBASE'], TSTSUFF)
    else:
        env['RELSRCDIR'] = os.path.join(* [os.pardir] * (len(TSTDIR.split(os.sep)) + 1) + [env['RELSRCBASE'], TSTDIR, TSTSUFF])
    os.environ['TSTDB']     = TSTDB
    os.environ['TSTDIR']    = TSTDIR
    os.environ['TSTSRCDIR'] = TSTSRCDIR
    os.environ['TSTTRGDIR'] = TSTTRGDIR
    os.environ['RELSRCDIR'] = env['RELSRCDIR']
    os.environ['PYTHON2']   = '/usr/bin/python2'
    os.environ['PYTHON2PATH'] = _configure(os.path.join('/usr/local', 'lib/python2.7/site-packages'))
    os.environ['PYTHON3']   = '/usr/bin/python3'
    os.environ['PYTHON3PATH'] = _configure(os.path.join('/usr/local', 'lib/python3.4/site-packages'))
    if os.name == 'nt':
        os.environ['PYTHON2PATH'] += os.path.pathsep + _configure(os.path.join('/usr/local', 'lib', 'python2'))
        os.environ['PYTHON3PATH'] += os.path.pathsep + _configure(os.path.join('/usr/local', 'lib', 'python3'))

    #STDERR.flush()
    #for v in 'RELSRCDIR':
    #       print v+" = "+str(env[v])
    #STDOUT.flush()

    if THISFILE == "Mtest.py":
        if env.has_key('GDK_DBFARM'):
            LogDBdir = os.path.join(env['GDK_DBFARM'],TSTDB)
            if not env.get('NOCLEAN') and LogDBdir and os.path.exists(LogDBdir):
                try:
                    shutil.rmtree(LogDBdir)
                except:
                    Warn("database '%s` exists, but destroying it failed; skipping tests in '%s`!" % (TSTDB, TSTSRCDIR))
                    #TODO:
                    # add "something" to HTML output
                    return td, elem, max(FdOut, FdErr)
            if os.path.isabs(LogDBdir) and not os.path.exists(LogDBdir):
                try:
                    os.makedirs(LogDBdir)
                except:
                    Warn("creating database '%s` failed; skipping tests in '%s`!" % (TSTDB, TSTSRCDIR))
                    #TODO:
                    # add "something" to HTML output
                    return td, elem, max(FdOut, FdErr)
        if not os.path.exists(TSTTRGDIR):
            #TODO: set mode to umask
            os.makedirs(TSTTRGDIR)

        body_good = []
        body_bad = []
        oktests = []
        if not verbose and not quiet:
            print '\nRunning in %s' % TSTDIR
        alllinks = []
        for TST,COND in testlist:
            os.environ['TST'] = TST
            tt, FtOut, FtErr, bodyline, reason, links = RunTest(env, TST, BusyPorts, COND, oktests, length)
            alllinks.extend(links)
            if tt:
                t = "%7.3f" % tt
            else:
                t = '-.---'
            TIMES.append((TSTDIR, TST, t, tt, FtOut, FtErr, reason))
            td += tt
            FdOut = max(FdOut,FtOut)
            FdErr = max(FdErr,FtErr)
            if bodyline is not None:
                if FtOut <= F_OK and FtErr <= F_OK:
                    body_good.append(bodyline)
                else:
                    body_bad.append(bodyline)
            if FtOut == F_OK and FtErr == F_OK:
                oktests.append(TST)
        TIMES.append((TSTDIR, '', "%7.3f" % td, td, FdOut, FdErr, None))
        if testweb:
            os.chdir(TSTTRGDIR)
            for f in alllinks:
                remove(f)

        if THISFILE == "Mtest.py":
            if not testweb:
                body = body_bad + body_good
                CreateHtmlIndex(env, *body)
                elem = AddSubToHtmlIndex(env, TSTDIR, max(FdOut,FdErr))

        # remove extra files created by tests
        for f in os.listdir(TSTTRGDIR):
            ff = os.path.join(TSTTRGDIR, f)
            if os.path.islink(ff):
                continue
            for pat in ['.Mapprove.rc', '.index.head.html', '.index.html',
                        'index.head.html', 'index.html',
                        'times.lst', 'times.sql',
                        '.*.nosrc.index.html',
                        '.*.src.index.head.html', '.*.src.index.html',
                        '*.FILTERED', '.*.SKIPPED',
                        '*.client.err', '*.client.out',
                        '*.err.diff.html', '*.err.head.html', '*.err.html',
                        '*.out.diff.html', '*.out.head.html', '*.out.html',
                        '*.server.err', '*.server.out',
                        '*.stable.err*', '*.stable.out*',
                        '*.test.err', '*.test.out']:
                if fnmatch.fnmatch(f, pat):
                    break
            else:
                remove(ff)

        if testweb:
            try:
                os.removedirs(TSTTRGDIR)
            except:
                pass

    else: # THISFILE == "Mapprove.py"
        if not os.path.exists(TSTTRGDIR):
            Warn("Output directory '%s` missing; skipping directory!" % TSTTRGDIR)
            return td, elem, max(FdOut, FdErr)

        for TST,COND in testlist:
            td += ApproveOutput(env, TST)

    return td, elem, max(FdOut, FdErr)
### PerformDir(env, testdir, testlist, BusyPorts) #

def ApproveOutput (env, TST) :
    sem = 0
    TSTDB = env['TSTDB']
    TSTDIR  = env['TSTDIR']
    TSTSRCDIR = env['TSTSRCDIR']
    TSTTRGDIR = env['TSTTRGDIR']
    os.chdir(TSTSRCDIR)
    EXTENSIONS = par['EXTENSION']
    FORCE = par['FORCE']
    NOPATCH = par['NOPATCH']

#       filter = re.compile( "^!WARNING: TCPlisten\([0-9]*\): stopped.$"        "|"
#                            "^!WARNING: TCPepilogue: terminate [01] listeners$", re.MULTILINE)

    TO = re.compile("(^\+(|[^#]*[\t ])((Memory|Segmentation) [Ff]ault|Bus [Ee]rror|Aborted|Assertion (|.* )failed[:\.]|!FATAL: BATSIGabort:|ERROR = !Connection terminated|!Mtimeout: Timeout:)([ \t]|$)|aborted too deep recursion)", re.MULTILINE)

    for WHAT in EXTENSIONS:
        testOUTPUT = os.path.join(TSTTRGDIR, "%s.test.%s" % (TST, WHAT))
        TSTSRCDIRTST = os.path.join(TSTSRCDIR, TST)
        stableOUT  = "%s.stable.%s" % (TSTSRCDIRTST, WHAT)
        if par['SYSTEM']:
            SYSTEM = par['SYSTEM']
            stableOUTPUT = stableOUT + SYSTEM
        else:
            if WHAT == 'out':
                w = 0
            else: # WHAT == 'err'
                w = 1
            stableOUTPUT = TSTSRCDIRTST + StableOutErr(env, par, TSTSRCDIRTST, SYST, RELEASE, DIST, VERSION)[w]
            SYSTEM = stableOUTPUT.split(WHAT)[-1]

        if os.path.isfile(testOUTPUT):
            if os.path.isfile(stableOUTPUT):
                oc = '   (overwriting old file)'
            else:
                oc = '   (creating new file)'
                if os.path.isfile(stableOUT):
                    shutil.copy(stableOUT,stableOUTPUT)
                else:
                    open(stableOUTPUT,"w").close()

            for d in ('TMPDIR', 'TMP', 'TEMP'):
                if os.environ.has_key(d):
                    patch = os.environ[d]
                    break
            else:
                patch = os.path.join(os.sep, 'tmp')
            patch = os.path.join(patch, "%s.patch-%s" % (os.path.basename(stableOUTPUT), str(os.getpid())))
            f = open(patch + '.0', 'w')
            proc = process.Popen(['diff', '-Bb', '-I^[#=]', '-U0',
                                  stableOUTPUT, testOUTPUT],
                                 stdout = f)
            proc.wait()
            f.close()
            if os.path.getsize(patch + ".0"):
                if not verbose:
                    oc = ''
                print "Approving  %s  ->  stable.%s%s%s" % (os.path.join(TSTDIR, "%s.test.%s" % (TST, WHAT)), WHAT, SYSTEM, oc)

                f = open(patch + ".1", "wb")
                for l in open(patch + ".0"):
                    if TO.search(l):
                        f.write(l[:1] + '\n')
                        Warn('Rejecting (error) message: "%s"' % l[1:].replace(os.linesep, ''))
                    elif len(l) < 2  or  \
                         (l[:2] not in ['+!','+='] and
                          not l.startswith('+ERROR = !') and
                          not l.startswith('+ERROR: ') and
                          not l.startswith('+WARNING: ')): # or  filter.match(ln):
                        f.write(l)
                    else:
                        if FORCE:
                            f.write(l)
                            sa = 'Approving'
                        else:
                            f.write(l[:1] + '\n')
                            sa = 'Skipping'
                        Warn('%s new (error) message: "%s"' % (sa,l[1:].replace(os.linesep, '')))
                        sem = 1
                f.flush()
                f.close()
                o = open(stableOUTPUT).read()
                open(stableOUTPUT + ".ORG", 'wb').write(o)
                open(stableOUTPUT, 'wb').write(o)
                patchcmd = ['patch']
                if not verbose:
                    patchcmd.append('--quiet')
                proc = process.Popen(patchcmd + [stableOUTPUT, patch + '.1'])
                proc.wait()
                f = open(patch, 'w')
                proc = process.Popen(['diff', '-u', stableOUTPUT + '.ORG',
                                      stableOUTPUT],
                                     stdout = f)
                proc.wait()
                f.close()
                remove(stableOUTPUT + ".ORG")
                remove(patch + ".1")
                o = open(stableOUTPUT).read()
                open(stableOUTPUT, 'w').write(o)
                o = None

                thefile = os.path.split(stableOUTPUT)[1]
                dir,file = os.path.split(stableOUT)
                test = re.compile('^%s.*$' % re.escape(file))
                list = []
                if not NOPATCH:
                    for f in os.listdir(dir or os.curdir):
                        if f.endswith('.rej') or f.endswith('.orig') or f.endswith('~'):
                            pass
                        elif f != thefile and test.match(f):
                            remove(os.path.join(dir or os.curdir, f + '.rej'))
                            remove(os.path.join(dir or os.curdir, f + '.orig'))
                            proc = process.Popen(patchcmd + ['--forward', os.path.join(dir or os.curdir, f)], stdin = open(patch))
                            proc.wait()
                            if os.path.exists(os.path.join(dir or os.curdir, f + '.rej')):
                                list.append(f)
                if len(list) > 0:
                    Warn('There are other (specific) stable outputs for test\n%s for which patching failed:\n  %s\n\n  Look at the *.rej files in directory %s.' % (os.path.join(TSTDIR,'Tests',TST), str(list), os.path.join(TSTDIR,'Tests')))
            elif verbose:
                print "No differences detected between  %s and  stable.%s%s  that are not ignored by Mtest.py." % (os.path.join(TSTDIR, "%s.test.%s" % (TST, WHAT)), WHAT, SYSTEM)
            remove(patch + ".0")
        elif verbose:
            i = TST.rfind('.')
            if i > 0:
                return ApproveOutput(env, TST[:i])
            Warn("Output file missing: '%s`; skipping test!" % testOUTPUT)
    return sem
### ApproveOutput (env, TST) #

# this function is a slightly modified copy of the posixpath version
# the differences are the doubling of \'s in the replacement value
# under a very specific condition: when the variable name starts with
# a Q and the variable name (with Q prefix) does not occur in the
# environment and the variable name minus the Q prefix does occur in
# the environment; and the addition of an extra parameter with default
# so that the environment which is used to expand can be replace.
_varprog = None
def expandvars(path, environ = os.environ):
    """Expand shell variables of form $var and ${var}.  Unknown variables
    are left unchanged."""
    global _varprog
    if '$' not in path:
        return path
    if not _varprog:
        import re
        _varprog = re.compile(r'\$(\w+|\{[^}]*\})')
    i = 0
    while True:
        m = _varprog.search(path, i)
        if not m:
            break
        i, j = m.span(0)
        name = m.group(1)
        if name.startswith('{') and name.endswith('}'):
            name = name[1:-1]
        if name in environ:
            tail = path[j:]
            val = environ[name]
            path = path[:i] + val
            i = len(path)
            path += tail
        elif name.startswith('Q') and name[1:] in environ:
            tail = path[j:]
            val = environ[name[1:]].replace('\\', '\\\\')
            path = path[:i] + val
            i = len(path)
            path += tail
        else:
            i = j
    return path

def returnCode(proc, f = None):
    '''Interpret the return code of a process.
    If second arg sepcified, write a message to it.'''
    if proc.killed:
        # don't write for timeout, killProc did that already
        return 'timeout'
    if os.name == 'nt':
        if proc.returncode == 3:
            # heuristic: abort() causes exit code 3
            if f is not None:
                f.write('\nAborted\n')
                f.flush()
            return 'abort'
        if proc.returncode == -1073741819: # 0xC0000005
            if f is not None:
                f.write('\nSegmentation fault\n')
                f.flush()
            return 'segfault'
        if proc.returncode == -1073741510: # 0xC000013A
            if f is not None:
                f.write('\nInterrupt\n')
                f.flush()
            return 'interrupt'  # Interrupt
        if proc.returncode != 0:
            return 'error'
    else:
        if proc.returncode == -signal.SIGSEGV:
            if f is not None:
                f.write('\nSegmentation fault\n')
                f.flush()
            return 'segfault'   # Segmentation fault
        if proc.returncode == -signal.SIGBUS:
            if f is not None:
                f.write('\nBus error\n')
                f.flush()
            return 'segfault'   # Bus error, treat as segfault
        if proc.returncode == -signal.SIGABRT:
            if f is not None:
                f.write('\nAborted\n')
                f.flush()
            return 'abort'      # Aborted
        if proc.returncode == -signal.SIGINT:
            if f is not None:
                f.write('\nInterrupt\n')
                f.flush()
            return 'interrupt'  # Interrupt
        if proc.returncode < 0:
            if f is not None:
                f.write('\nSignal %d\n' % -proc.returncode)
                f.flush()
            return 'signal'     # some other signal
        if proc.returncode > 0:
            return 'error'
    return None                 # no error

def GetBitsAndOIDsAndModsAndStaticAndThreads(env) :
    global setpgrp
    rtrn = 0
    cmd = splitcommand(env['exe']['Mserver'][1])
    cmd.append('--dbpath=%s' % os.path.join(env['GDK_DBFARM'], TSTPREF))
    if env.get('MULTIFARM'):
        cmd.append('--dbextra=%s' % os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'))
        shutil.rmtree(os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'),
                      ignore_errors = True)
        os.makedirs(os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'))
    if procdebug:
        print 'GetBitsAndOIDsAndModsAndStaticAndThreads: starting process "%s" (inpipe, outpipe, errpipe)\n' % '" "'.join(cmd)
    setpgrp = True
    proc = process.Popen(cmd, stdin = process.PIPE, stdout = process.PIPE,
                         stderr = process.PIPE, universal_newlines = True)
    proc.killed = False
    t = Timer(float(par['TIMEOUT']), killProc, args = [proc, proc.stderr, cmd])
    try:
        t.start()
        input = '''\
                c := mdb.modules();
                modsid := algebra.subunique(c);
                mods := algebra.leftfetchjoin(modsid,c);
                s := "\\nModules: ";
                sep := "";
                barrier (h:oid,t:str) := iterator.new(mods);
                        s := s + sep;
                        s := s + "\'";
                        s := s + t;
                        s := s + "\'";
                        sep := ",";
                redo (h:oid,t:str) := iterator.next(mods);
                exit h;
                s := s + "\\n";
                io.printf(s);
                clients.quit();
            '''
        ##module("NoModule");
        qOut, qErr = proc.communicate(input = input)
        t.cancel()
        if procdebug:
            print 'GetBitsAndOIDsAndModsAndStaticAndThreads: process exited "%s" (%s)\n' % ('" "'.join(cmd), proc.returncode)
    except KeyboardInterrupt:
        t.cancel()
        killProc(proc, proc.stderr, cmd)
        if procdebug:
            print 'GetBitsAndOIDsAndModsAndStaticAndThreads: process killed "%s"\n' % '" "'.join(cmd)
        raise
    returncode = returnCode(proc)
    if returncode is not None:
        STDERR.write(' '.join(cmd) + "\n\n")
        if input:
            STDERR.write(input)
            STDERR.write("\n")
        if qOut:
            STDERR.write(qOut)
            STDERR.write("\n")
        if qErr:
            STDERR.write(qErr)
            STDERR.write("\n")
        STDERR.flush()
        ErrExit('GetBitsAndOIDsAndModsAndStaticAndThreads: subcommand failed: %s' % returncode)
    env['TST_MODS'] = []
    env['TST_BITS'] = ""
    env['TST_OIDS'] = ""
    env['TST_INT128'] = ""
    env['TST_STATIC'] = ""
    env['TST_SINGLE'] = ""
    env['TST_THREADS'] = ""
    if qOut:
        tbos = re.compile("^# Compiled for .*/([63][42]bit) with ([63][42])bit OIDs;?(| and 128bit integers) ([^ ]*) linked", re.MULTILINE)
        tt = re.compile("^# Serving database .*, using ([0-9]+) threads?", re.MULTILINE)
        tm = re.compile("^Modules: (.+)$", re.MULTILINE)
        #ts = re.compile("^!ERROR: DL_open: library not found \(STATIC\).$", re.MULTILINE)
        for l in qOut.split('\n'):
            obs = tbos.match(l)
            if obs:
                env['TST_BITS'] = obs.group(1)
                os.environ['TST_BITS'] = env['TST_BITS']
                env['TST_OIDS'] = "oid" + obs.group(2)
                os.environ['TST_OIDS'] = env['TST_OIDS']
                if obs.group(3) == " and 128bit integers":
                    env['TST_INT128'] = "int128"
                    os.environ['TST_INT128'] = env['TST_INT128']
                if obs.group(4) == "statically":
                    env['TST_STATIC'] = "STATIC"
                    os.environ['TST_STATIC'] = env['TST_STATIC']
            t = tt.match(l)
            if t:
                if t.group(1) == "1":
                    env['TST_SINGLE'] = "single"
                    os.environ['TST_SINGLE'] = env['TST_SINGLE']
                env['TST_THREADS'] = t.group(1)
            m = tm.match(l)
            if m:
                env['TST_MODS'] = eval(m.group(1))
            #s = ts.match(l)
            #if s:
            #       env['TST_STATIC'] = "1"
            #       os.environ['TST_STATIC'] = env['TST_STATIC']
        if not env['TST_BITS']:
            ErrMsg("Checking for Bits failed!")
        if not env['TST_OIDS']:
            ErrMsg("Checking for OIDs failed!")
        if not env['TST_MODS']:
            ErrMsg("Checking for Modules failed!")
        if not env['TST_BITS'] or not env['TST_OIDS'] or not env['TST_MODS']:
            STDERR.write(' '.join(cmd) + "\n\n")
            STDERR.write(qOut)
            STDERR.write("\n")
            STDERR.write(qErr)
            STDERR.write("\n")
            STDERR.flush()
            rtrn = 1
    else:
        rtrn = 1
        ErrMsg("No output from Mserver/mserver5 when checking for Bits, OIDs, Modules & Threads!?")
        if qErr:
            STDERR.write(' '.join(cmd) + "\n\n")
            STDERR.write(qErr)
            STDERR.write("\n")
            STDERR.flush()
    os.environ['TST_MODS'] = str(env['TST_MODS'])
    return rtrn
### GetBitsAndOIDsAndModsAndStaticAndThreads(env) #

def CheckMods(env, TST, SERVER, CALL) :
    missing = []
    if os.path.isfile(TST + ".modules"):
        for m in open(TST + ".modules"):
            m = m.strip()
            if m  and  m[0] != "#"  and  m not in env['TST_MODS']:
                missing.append(m)
    if SERVER == "SQL":
        sql_mods = ["sql"]
        for m in sql_mods:
            if m not in env['TST_MODS']:
                missing.append(m)
    return missing
### CheckMods(env, TST, SERVER, CALL) #

def CheckTests(env, TST, oktests):
    missing = []
    if not os.path.isfile(TST + '.reqtests'):
        # no required tests, so none missing
        return missing
    if env.get('NOCLEAN'):
        # we didn't clean up from a previous run, assume tests were done
        return missing

    for test in open(TST + '.reqtests'):
        test = test.strip()
        if not test or test.startswith('#'):
            continue
        if not test in oktests:
            missing.append(test)
    return missing
### CheckTests(env, TST, oktests) #

def StableOutErr(env,par,TST,SYST,RELEASE,DIST,VERSION) :
    BITS = env['TST_BITS']
    OIDS = env['TST_OIDS']
    INT128 = env['TST_INT128']
    if INT128:
        INT128 = r"(\.int128)?"
    SINGLE = env['TST_SINGLE']
    if SINGLE:
        SINGLE = r"(\.single)?"
    STATIC = env['TST_STATIC']
    if STATIC:
        STATIC = r"(\.STATIC)?"
    dir,file = os.path.split(TST)
    outre = re.compile(r'^%s\.stable\.(?P<tp>out|err)(\.(%s(%s)?|%s(%s)?))?(\.%s)?(\.%s)?%s%s%s$' % (re.escape(file), re.escape(SYST), re.escape(RELEASE), re.escape(DIST), re.escape(VERSION), BITS, OIDS, INT128, SINGLE, STATIC))
    bestout = besterr = ''
    for f in os.listdir(dir or os.curdir):
        res = outre.match(f)
        if res is not None:
            if res.group('tp') == 'out':
                if len(bestout) < len(f):
                    bestout = f
            else:                   # res.group('tp') == 'err'
                if len(besterr) < len(f):
                    besterr = f
    if bestout:
        STABLEout = os.path.join(dir, bestout)[len(TST):]
    else:
        STABLEout = '.stable.out'
    if besterr:
        STABLEerr = os.path.join(dir, besterr)[len(TST):]
    else:
        STABLEerr = '.stable.err'
    return STABLEout, STABLEerr
### StableOutErr(env,par,TST,SYST,RELEASE,DIST,VERSION) #

def CategorizeResult(TST, SockTime):
    l = '<!--MajorDiffs-->'   # assign something in case file is empty
    for l in open("%s.out.diff.html" % TST):
        pass
    if   l.startswith('<!--NoDiffs-->'):
        o = F_OK
    elif l.startswith('<!--MinorDiffs-->'):
        o = F_WARN
    elif l.startswith('<!--MajorDiffs-->'):
        o = F_ERROR
    else:
        Warn("Unexpected last line in %s.out.diff.html:\n%s" % (TST, l))
        ff = open("%s.out.diff.html" % TST, "a")
        ff.write("\n<!--MajorDiffs-->\n")
        ff.close()
        o = F_ERROR
    l = '<!--MajorDiffs-->'   # assign something in case file is empty
    for l in open("%s.err.diff.html" % TST):
        pass
    if   l.startswith('<!--NoDiffs-->'):
        e = F_OK
    elif l.startswith('<!--MinorDiffs-->'):
        e = F_WARN
    elif l.startswith('<!--MajorDiffs-->'):
        e = F_ERROR
    else:
        Warn("Unexpected last line in %s.err.diff.html:\n%s" % (TST, l))
        ff = open("%s.err.diff.html" % TST, "a")
        ff.write("\n<!--MajorDiffs-->\n")
        ff.close()
        e = F_ERROR
    if e == F_ERROR and SockTime in (F_SOCK, F_TIME, F_RECU, F_ABRT, F_SEGV):
        e = SockTime
    return o, e

def RunTest(env, TST, BusyPorts, COND, oktests, length) :
    global setpgrp
    Failed = F_SKIP
    FailedOut = F_SKIP
    FailedErr = F_SKIP
    TSTDB = env['TSTDB']
    TSTDIR  = env['TSTDIR']
    TSTSRCDIR = env['TSTSRCDIR']
    RELSRCDIR = env['RELSRCDIR']
    TSTTRGDIR = env['TSTTRGDIR']
    os.chdir(TSTSRCDIR)
    elem = None
    reason = None               # reason for skipping (if any)
    links = []                  # symlinks we make

    TX = 0
    EXT = CALL = SERVER = ""
    x  = isexecutable(TST)
    if not x[0]:
        x  = isexecutable(TST,'')
    xA = isexecutable(TST + ".MAL")
    xS = isexecutable(TST + ".SQL")
    if   x[0]:
        EXT = x[1]
        CALL = "other"
    elif xA[0]:
        EXT = ".MAL"+xA[1]
        CALL = "other"
        SERVER = "MAL"
    elif xS[0]:
        EXT = ".SQL"+xS[1]
        CALL = "other"
        SERVER = "SQL"
    elif os.path.isfile(TST+".py")            or  os.path.isfile(TST+".py.src"):
        EXT = ".py"
        CALL = "python"
    elif os.path.isfile(TST+".MAL"+".py")     or  os.path.isfile(TST+".MAL"+".py.src")     or  os.path.isfile(TST+".MAL"+".py.in"):
        EXT = ".MAL.py"
        CALL = "python"
        SERVER = "MAL"
    elif os.path.isfile(TST+".SQL"+".py")     or  os.path.isfile(TST+".SQL"+".py.src")     or  os.path.isfile(TST+".SQL"+".py.in"):
        EXT = ".SQL.py"
        CALL = "python"
        SERVER = "SQL"
    elif os.path.isfile(TST+".mal")           or  os.path.isfile(TST+".mal.src")           or  os.path.isfile(TST+".mal.in"):
        EXT = ".mal"
        CALL = "mal"
    elif os.path.isfile(TST+"_s00.mal")       or  os.path.isfile(TST+"_s00.mal.src")       or  os.path.isfile(TST+"_s00.mal.in"):
        EXT = ".mal"
        CALL = "malXs"
    elif os.path.isfile(TST+".malC")          or  os.path.isfile(TST+".malC.src")          or  os.path.isfile(TST+".malC.in"):
        EXT = ".malC"
        CALL = "malC"
        SERVER = "MAL"
    elif os.path.isfile(TST+"_s00.malC")      or  os.path.isfile(TST+"_s00.malC.src")      or  os.path.isfile(TST+"_s00.malC.in"):
        EXT = ".malC"
        CALL = "malCXs"
        SERVER = "MAL"
    elif os.path.isfile(TST+"_p00.malC")      or  os.path.isfile(TST+"_p00.malC.src")      or  os.path.isfile(TST+"_p00.malC.in"):
        EXT = ".malC"
        CALL = "malCXp"
        SERVER = "MAL"
    elif os.path.isfile(TST+".sql")           or  os.path.isfile(TST+".sql.src")           or  os.path.isfile(TST+".sql.in"):
        EXT = ".sql"
        CALL = "sql"
        SERVER = "SQL"
    elif os.path.isfile(TST+"_s00.sql")       or  os.path.isfile(TST+"_s00.sql.src")       or  os.path.isfile(TST+"_s00.sql.in"):
        EXT = ".sql"
        CALL = "sqlXs"
        SERVER = "SQL"
    elif os.path.isfile(TST+"_p00.sql")       or  os.path.isfile(TST+"_p00.sql.src")       or  os.path.isfile(TST+"_p00.sql.in"):
        EXT = ".sql"
        CALL = "sqlXp"
        SERVER = "SQL"
    elif os.path.isfile(TST+".R"):
        EXT = ".R"
        CALL = "R"
        SERVER = "SQL"

        #TODO:
        #elif [ -f "$TST.java"       ] ; then  EXT="java" ; CALL="Java   "+TST+" "+EXT
        #elif [ -f "${TST}_s00.java" ] ; then  EXT="java" ; CALL="JavaXs "+TST+" "+EXT
        #elif [ -f "${TST}_p00.java" ] ; then  EXT="java" ; CALL="JavaXp "+TST+" "+EXT
        #elif [ -f "$TST.odmg"       ] ; then  EXT="odmg" ; CALL="odmg   "+TST+" "+EXT
    else:
        os.chdir(TSTTRGDIR)
        i = TST.rfind('.')
        if i > 0:
            return RunTest(env, TST[:i], BusyPorts, COND, oktests, length)
        EXT = CALL = SERVER = ""
        if COND:
            for cond in COND.split('&'):
                if cond.startswith('!'):
                    negate = True
                    cond = cond[1:]
                else:
                    negate = False
                if cond == 'PREVREL':
                    if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevrel.zip')):
                        reason = "as previous release database is not available"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond == 'PREVHGEREL':
                    if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevhgerel.zip')):
                        reason = "as previous hugeint release database is not available"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond == 'PREVCHAINREL':
                    if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevchainrel.zip')):
                        reason = "as previous chained release database is not available"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond == 'PREVHGECHAINREL':
                    if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevhgechainrel.zip')):
                        reason = "as previous hugeint chained release database is not available"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond.startswith('THREADS='):
                    if (env['TST_THREADS'] == cond[8:]) == negate:
                        reason = "as number of threads is wrong"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond.startswith('THREADS<='):
                    if (int(env['TST_THREADS']) <= int(cond[9:])) == negate:
                        reason = "as number of threads is wrong"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif cond.startswith('THREADS>='):
                    if (int(env['TST_THREADS']) >= int(cond[9:])) == negate:
                        reason = "as number of threads is wrong"
                        elem = SkipTest(env, TST, EXT, reason, length)
                        break
                elif not CONDITIONALS.has_key(cond):
                    reason = "as conditional '%s' is unknown." % cond
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
                elif (not CONDITIONALS[cond]) != negate:
                    if negate:
                        reason = "as conditional '%s' holds." % cond
                    else:
                        reason = "as conditional '%s' does not hold." % cond
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
        if reason:
            pass
        elif os.name == "nt":
            ErrMsg("test missing: '"+os.path.join(TSTSRCDIR,TST)+".(exe|com|bat|cmd|py|mal|malC|sql)`")
            #TODO:
            #elif os.name == "posix":
        else:
            ErrMsg("test missing: '"+os.path.join(TSTSRCDIR,TST)+"[.py|.mal|.malC|.sql|.R]`")
        return TX,Failed,Failed,elem,reason,links

    MissingMods = CheckMods(env, TST, SERVER, CALL)
    MissingTests = CheckTests(env, TST, oktests)
    nomito = os.path.isfile(TST + '.nomito')


    os.chdir(TSTTRGDIR)

    if COND:
        for cond in COND.split('&'):
            if cond.startswith('!'):
                negate = True
                cond = cond[1:]
            else:
                negate = False
            if cond == 'PREVREL':
                if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevrel.zip')):
                    reason = "as previous release database is not available"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond == 'PREVHGEREL':
                if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevhgerel.zip')):
                    reason = "as previous hugeint release database is not available"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond == 'PREVCHAINREL':
                if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevchainrel.zip')):
                    reason = "as previous chained release database is not available"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond == 'PREVHGECHAINREL':
                if not os.path.exists(os.path.join(env['GDK_DBFARM'], 'prevhgechainrel.zip')):
                    reason = "as previous hugeint chained release database is not available"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond.startswith('THREADS='):
                if (env['TST_THREADS'] == cond[8:]) == negate:
                    reason = "as number of threads is wrong"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond.startswith('THREADS<='):
                if (int(env['TST_THREADS']) <= int(cond[9:])) == negate:
                    reason = "as number of threads is wrong"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif cond.startswith('THREADS>='):
                if (int(env['TST_THREADS']) >= int(cond[9:])) == negate:
                    reason = "as number of threads is wrong"
                    elem = SkipTest(env, TST, EXT, reason, length)
                    break
            elif not CONDITIONALS.has_key(cond):
                reason = "as conditional '%s' is unknown." % cond
                elem = SkipTest(env, TST, EXT, reason, length)
                break
            elif (not CONDITIONALS[cond]) != negate:
                if negate:
                    reason = "as conditional '%s' holds." % cond
                else:
                    reason = "as conditional '%s' does not hold." % cond
                elem = SkipTest(env, TST, EXT, reason, length)
                break
    if reason:
        pass
    elif MissingTests:
        reason = "as required test%s '%s' failed." % (len(MissingTests) != 1 and 's' or '', "', '".join(MissingTests))
        elem = SkipTest(env, TST, EXT, reason, length)
    elif EXT == ".malC" and  not env['exe']['MAL_Client'][0]:
        reason = "as %s is not available." % env['MALCLIENT'].split(None, 1)[0]
        elem = SkipTest(env, TST, EXT, reason, length)
    elif EXT == ".sql" and  not env['exe']['SQL_Client'][0]:
        reason = "as %s is not available." % env['SQLCLIENT'].split(None, 1)[0]
        elem = SkipTest(env, TST, EXT, reason, length)
    elif EXT == ".sql" and  not env['exe']['SQL_Dump'][0]:
        reason = "as %s is not available." % env['SQLDUMP'].split(None, 1)[0]
        elem = SkipTest(env, TST, EXT, reason, length)
    elif SERVER in ["MAL", "SQL"] and not env['exe']['Mserver'][0]:
        reason = "as %s is not available." % env['MSERVER'].split(None, 1)[0]
        elem = SkipTest(env, TST, EXT, reason, length)
    elif EXT == ".malS" and not env['exe']['Mserver'][0]:
        reason = "as %s is not available." % env['MSERVER'].split(None, 1)[0]
        elem = SkipTest(env, TST, EXT, reason, length)
    elif CALL == "python"  and  not env['exe']['python'][0]:
        reason = "as python is not available."
        elem = SkipTest(env, TST, EXT, reason, length)
        #TODO:
        #elif [ "$EXT" = "java"  -a  ! "`type -path java`" ] ; then
        #elem = SkipTest(env, TST, EXT, "as java is not in $PATH.", length)
    elif MissingMods:
        reason = "as modules '%s` are missing." % str(MissingMods)
        elem = SkipTest(env, TST, EXT, reason, length)
    elif CALL == "malCXp":
        reason = "as multiple MAL clients in parallel are currently not supported by %s." % THISFILE
        elem = SkipTest(env, TST, EXT, reason, length)
    elif CALL == "sqlXp":
        reason = "as multiple SQL clients in parallel are currently not supported by %s." % THISFILE
        elem = SkipTest(env, TST, EXT, reason, length)
    elif SERVER in ["MAL", "SQL"] and "MAPI" in BusyPorts:
        reason = "as MAPIPORT=%s is not available." % env['MAPIPORT']
        elem = SkipTest(env, TST, EXT, reason, length)
    else:
        test = re.compile("^"+TST+"((_[sp][0-9][0-9])?\..*)?$", re.MULTILINE)
        for f in os.listdir(RELSRCDIR):
            if test.match(f):
                try:
                    SymlinkOrCopy(os.path.join(RELSRCDIR, f), f)
                    links.append(os.path.join(TSTTRGDIR, f))
                except IOError, (IOerrNo, IOerrStr):
                    if not env.get('NOCLEAN'):
                        ErrMsg("SymlinkOrCopy('%s','%s') in '%s' failed with #%d: '%s'."
                               % (os.path.join(RELSRCDIR, f), f, os.getcwd(), IOerrNo, IOerrStr))
                except OSError:
                    if not env.get('NOCLEAN'):
                        raise

        # Check for available sockets and block them until we're ready to run the actual test
        MAPIsockets, reason = CheckSocket2(env, "MAPI")   #, SrvrErr)
        if MAPIsockets is None:
            reason = 'as ' + reason
            elem = SkipTest(env, TST, EXT, reason, length)
            return TX,Failed,Failed,elem,reason,links

        if os.path.isfile(TST+EXT+".src")  and not os.path.isfile(TST+EXT):
            f = open(TST+EXT+".src","r")
            TSTSRC = expandvars(path(f.readline().strip()), env)
            f.close()
            if os.path.isfile(TSTSRC):
                try:
                    SymlinkOrCopy(TSTSRC, TST + EXT)
                    links.append(TST + EXT)
                except IOError, (IOerrNo, IOerrStr):
                    ErrMsg("SymlinkOrCopy('%s','%s') in '%s' failed with #%d: '%s'."
                           % (TSTSRC, TST + EXT, os.getcwd(), IOerrNo, IOerrStr))
            else:
                reason = "as source file '%s` is missing." % TSTSRC
                elem = SkipTest(env, TST, EXT+".src", reason, length)
                # Release reserved sockets before bailing out
                MAPIsockets[0].close()
                MAPIsockets[1].close()
                return TX,Failed,Failed,elem,reason,links
        test = re.compile("^"+TST+"((_[sp][0-9][0-9])?\..*)?\.src$", re.MULTILINE)
        for ff in os.listdir(TSTTRGDIR):
            if test.match(ff) and not os.path.isfile(ff[:-4]):
                f = open(ff,"r")
                TSTSRC = expandvars(path(f.readline().strip()), env)
                f.close()
                if os.path.isfile(TSTSRC):
                    try:
                        SymlinkOrCopy(TSTSRC, ff[:-4])
                        links.append(ff[:-4])
                    except IOError, (IOerrNo, IOerrStr):
                        ErrMsg("SymlinkOrCopy('%s','%s') in '%s' failed with #%d: '%s'."
                               % (TSTSRC, ff[:-4], os.getcwd(), IOerrNo, IOerrStr))
                else:
                    Warn("source file '"+TSTSRC+"` is missing.")
        test = re.compile("^"+TST+"(_[sp][0-9][0-9])?\..*\.in$", re.MULTILINE)
        for ff in os.listdir(TSTTRGDIR):
            fff = ff[:-3]
            if test.match(ff) and not os.path.isfile(fff):
                f = open(fff,"w")
                for l in open(ff):
                    f.write(expandvars(l, env))
                f.close()

        ACCURACYout = par['ACCURACY']
        ACCURACYerr = par['ACCURACY']
        STABLEout,STABLEerr = StableOutErr(env,par,TST,SYST,RELEASE,DIST,VERSION)
        if not os.path.isfile(TST+STABLEout):
            open(TST+STABLEout,"w").close()
            ACCURACYout = 0
        if not os.path.isfile(TST+STABLEerr):
            open(TST+STABLEerr,"w").close()
            ACCURACYerr = 0

        PRELUDE = []
        if EXT !=  '.sql':
            if os.path.isfile(TST+".prelude5"):
                PRELUDE = [TST+".prelude5"]

        TIMEOUT = par['TIMEOUT']
        if os.path.isfile(TST+".timeout"):
            for f in open(TST+".timeout"):
                TOf = float(f.strip())
                if TOf > 0:
                    TIMEOUT = int(TIMEOUT * TOf)
                if TIMEOUT < 1 and par['TIMEOUT'] > 0:
                    TIMEOUT = 1
        if env['exe']['Mtimeout'][0]:
            # set timeout
            env['exe']['Mtimeout'] = env['exe']['Mtimeout'][0], 'Mtimeout -timeout %s ' % str(TIMEOUT)
            SetExecEnv(env['exe'],False)
        CTIMEOUT = 0
        if   CALL in ["other", "python"]:
            if TIMEOUT > 0:
                CTIMEOUT = CTIMEOUT + min(TIMEOUT, par['TIMEOUT'])
        elif CALL in ["malXs", "sqlXs"]:
            test = re.compile("^"+TST+"_s[0-9][0-9]"+EXT+"$", re.MULTILINE)
            d = os.listdir(os.getcwd())
            for f in d:
                if test.match(f):
                    CTIMEOUT = CTIMEOUT + TIMEOUT
        elif CALL in ["mal", "malC", "sql"]:
            CTIMEOUT = CTIMEOUT + TIMEOUT
        if  CTIMEOUT < TIMEOUT:
            CTIMEOUT = TIMEOUT
        STIMEOUT = CTIMEOUT
        if  SERVER in ["MAL", "SQL"] and TIMEOUT > 0:
            STIMEOUT = STIMEOUT + TIMEOUT + min(TIMEOUT, par['TIMEOUT'])

        ME = ""

        TestOutFile = TST+".test.out"
        TestErrFile = TST+".test.err"
        TestOut = open(TestOutFile,"w")
        TestErr = open(TestErrFile,"w")
        TestOut.write("stdout of test '"+TST+"` in directory '"+url(TSTDIR)+"` itself:\n\n")
        TestErr.write("stderr of test '"+TST+"` in directory '"+url(TSTDIR)+"` itself:\n\n")
        TestOut.close()
        TestErr.close()

        t0 = time.time()
        tres = DoIt(env, SERVER, CALL, TST, EXT, PRELUDE, TestOutFile, TestErrFile, STIMEOUT, CTIMEOUT, TIMEOUT, ME, MAPIsockets, length, nomito)
        if tres == 'segfault':
            # rename core file, if any -- might have to check
            # /proc/sys/kernel/core_pattern in the future but hopefully
            # this value is kept sane
            corefile = os.path.join(env['GDK_DBFARM'], env['TSTDB'], 'core')
            if not os.path.exists(corefile):
                corefile = None
            # braindead engineers at RedHat decided to change the core
            # dump name on their own (disregarding kernel settings)
            if corefile is None:
                corefile = os.path.join(env['GDK_DBFARM'], env['TSTDB'], 'core.*')
                corefile = glob.glob(corefile)
                if corefile is not None and len(corefile) > 0 and os.path.exists(corefile[0]):
                    corefile = corefile[0]
                else:
                    corefile = None
            # FreeBSD uses processname.core
            if corefile is None:
                corefile = os.path.join(env['GDK_DBFARM'], env['TSTDB'], 'mserver5.core')
                if not os.path.exists(corefile):
                    corefile = None
            if corefile is not None:
                try:
                    os.rename(corefile, os.path.join(env['GDK_DBFARM'],
                        env['TSTDB'], 'core-%s' % (TST)))
                except:
                    corefile = None
            # leave a marker for TestTools
            if corefile is None:
                try:
                    f = open(os.path.join(env['GDK_DBFARM'], env['TSTDB'],
                        'crash-%s' % (TST)))
                    f.write("crash without core file, or renaming it failed\n")
                    f.close()
                except:
                    pass

        t1 = time.time()
        TX = t1 - t0
        if not quiet:
            STDOUT.write(" %7.3fs " % TX)

        errcode = F_OK
        if tres == 'timeout':
            errcode = F_TIME
        elif tres == 'recursion':
            errcode = F_RECU
        elif tres == 'segfault':
            errcode = F_SEGV
        elif tres == 'abort':
            errcode = F_ABRT
        elif tres == 'socket':
            errcode = F_SOCK
        elif tres == 'error':
            errcode = F_WARN
        elif tres is not None:
            errcode = F_ERROR

        sockerr = CheckSocket3(env, "MAPI", TestErrFile)

        #TODO:
        ##if [ ! -f $TSTTRGBASE/Tests/.old.left-over.tmp.bats. ] ; then  touch $TSTTRGBASE/Tests/.old.left-over.tmp.bats. ; fi
        ##LEFTOVERTMPBATS="`find $MONETDBFARM/dbfarm/*/bat/ -name tmp_\* -print 2> /dev/null`"
        ##if [ "$LEFTOVERTMPBATS" ] ; then
        ##      ls -alF $LEFTOVERTMPBATS 2> /dev/null > .all.left-over.tmp.bats.
        ##      diff -u0 $TSTTRGBASE/Tests/.old.left-over.tmp.bats. .all.left-over.tmp.bats. | grep '^\+[^\+]' > .new.left-over.tmp.bats.
        ##fi
        ##if [ -s .new.left-over.tmp.bats. ] ; then
        ##      echo -e "\n!ERROR: persistent temporary bats remained:" >> $LOGFILE.err
        ##      sed 's|^\+|! |g' .new.left-over.tmp.bats.               >> $LOGFILE.err
        ##      echo                                                    >> $LOGFILE.err
        ##fi
        ##rm -f .new.left-over.tmp.bats. $TSTTRGBASE/Tests/.old.left-over.tmp.bats.
        ##if [ -f .all.left-over.tmp.bats. ] ; then  mv -f .all.left-over.tmp.bats. $TSTTRGBASE/Tests/.old.left-over.tmp.bats. ; fi

        if tres == 'socket':
            if quiet:
                STDOUT.write("\n%s : Socket!\n" % TST)
            elif verbose:
                STDOUT.write("(Socket!) ")

        if tres == 'timeout':
            if quiet:
                STDOUT.write("\n%s : Timeout!\n" % TST)
            elif verbose:
                STDOUT.write("(Timeout!) ")

        if tres == 'recursion':
            if quiet:
                STDOUT.write("\n%s : Recursion!\n" % TST)
            elif verbose:
                STDOUT.write("(Recursion!) ")

        if tres == 'segfault':
            if quiet:
                STDOUT.write("\n%s : Crashed!\n" % TST)
            elif verbose:
                STDOUT.write("(Crashed!) ")

        if tres == 'signal':
            if quiet:
                STDOUT.write("\n%s : Signaled!\n" % TST)
            elif verbose:
                STDOUT.write("(Signaled!) ")

        if verbose:
            STDOUT.write("\n")

        try:
            STDOUT.flush()
        except IOError, (IOerrNo, IOerrStr):
            Warn("Flushing STDOUT in RunTest failed with #%d: '%s'." % (IOerrNo, IOerrStr))

        if env['exe']['Mtimeout'][0]:
            # reset timeout
            env['exe']['Mtimeout'] = env['exe']['Mtimeout'][0], 'Mtimeout -timeout %s ' % str(par['TIMEOUT'])
            SetExecEnv(env['exe'],False)

        try:
            Mfilter.mFilter(TST+STABLEout,par['IGNORE'])
            Mfilter.mFilter(TST+STABLEerr,par['IGNORE'])
            Mfilter.mFilter(TST+".test.out",par['IGNORE'])
            Mfilter.mFilter(TST+".test.err",par['IGNORE'])
        except:
            Warn("mFilter failed\n")
            pass

        if REV:
            d = '%s%s/%s' % (URLPREFIX, url(TSTDIR), TSTSUFF)
            if os.path.isfile(TST + EXT + '.src'):
                f = open(TST + EXT + '.src').readline().strip()
                if f.startswith('$RELSRCDIR/'):
                    f = f[11:]
                    while f.startswith('../'):
                        f = f[3:]
                        d = d[:d.rindex('/')]
                f = '%s/%s' % (d, f)
            elif os.path.isfile(TST + EXT + '.in'):
                f = '%s/%s%s.in' % (d, TST, EXT)
            else:
                f = '%s/%s%s' % (d, TST, EXT)
            if testweb:
                # splice in a link to the bug report if we recognize a
                # reference
                res = bugre.search(TST)
                if res is not None:
                    bugno = res.group('bugno')
                    tst = '%s<a target="_blank" href="http://bugs.monetdb.org/%s">%s</a>%s' % (TST[:res.start(0)+1], bugno, res.group(0)[1:], TST[res.end(0):])
                else:
                    tst = TST
                titlefmt = '-tTest %s%s (id <a href="%s">%s</a>) (<a href="%s.%%s.diff.html">%%s</a>)' % (tst, EXT, f, REV, TST)
            else:
                # no need (and no space) to add link to bug report:
                # it's done already elsewhere
                titlefmt = '-tTest <a href="%s%s">%s%s</a> (id <a href="%s">%s</a>) (<a href="%s.%%s.diff.html">%%s</a>)' % (TST, EXT, TST, EXT, f, REV, TST)
        elif testweb:
            titlefmt = '-tTest %s%s (<a href="%s.%%s.diff.html">%%s</a>)' % (TST, EXT, TST)
        else:
            titlefmt = '-tTest <a href="%s%s">%s%s</a> (<a href="%s.%%s.diff.html">%%s</a>)' % (TST, EXT, TST, EXT, TST)
        diff_html = open('%s.out.diff.html' % TST,"w")
        diff_html.write('<!--MajorDiffs-->\n')
        diff_html.close()
        timedout = True
        if tres is not None:
            # test program exited with error => expect major differences!
            ACCURACYout = -1
        else:
            fs = open("%s%s.FILTERED" % (TST, STABLEout))
            ft = open("%s.test.out.FILTERED" % TST)
            szs = os.fstat(fs.fileno())[6]
            szt = os.fstat(ft.fileno())[6]
            fs.close()
            ft.close()
            if szt < szs*0.5 or szt > szs*1.5:
                # filesizes differ significantly => expect major differences!
                ACCURACYout = -1
        out = err = ''
        while timedout:
            cmd = ['Mdiff']
            if ACCURACYout == -1:
                ACCURACYout = 0
            else:
                cmd.append('-d')
            if not verbose:
                cmd.append('-q')
            cmd.extend(['-F^#', '-I%s' % par['IGNORE'],
                        '-C%s' % par['CONTEXT'], '-A%d' % ACCURACYout,
                        titlefmt % ('err', 'err'),
                        '%s%s.FILTERED' % (TST, STABLEout),
                        '%s.test.out.FILTERED' % TST,
                        '%s.out.diff.html' % TST])
            if procdebug:
                print 'RunTest: starting process "%s"\n' % '" "'.join(cmd)
            setpgrp = True
            proc = process.Popen(cmd, stdout = process.PIPE,
                                 stderr = process.PIPE)
            proc.killed = False
            t = Timer(float(par['TIMEOUT']), killProc, args = [proc])
            try:
                t.start()
                out, err = proc.communicate()
                t.cancel()
                if verbose or quiet:
                    if out:
                        STDOUT.write(out)
                    if err:
                        sys.stderr.write(err)
                if procdebug:
                    print 'RunTest: process exited "%s" (%s)\n' % \
                        ('" "'.join(cmd), proc.returncode)
            except KeyboardInterrupt:
                t.cancel()
                killProc(proc)
                if procdebug:
                    print 'RunTest: process killed "%s"\n' % '" "'.join(cmd)
                raise
            timedout = proc.killed
            ACCURACYout = ACCURACYout - 1
            if ACCURACYout < 0:
                timedout = False # don't try again
        if env.get('ECHO_DIFF'):
            cmd = ['diff']
            if ACCURACYout >= 0:
                cmd.append('-d')
            cmd.extend(['-Bb', '-F^#', '-I%s' % par['IGNORE'],
                        '-U%s' % par['CONTEXT'],
                        '%s%s.FILTERED' % (TST, STABLEout),
                        '%s.test.out.FILTERED' % TST])
            proc = process.Popen(cmd)
            proc.wait()

        diff_html = open('%s.err.diff.html' % TST,"w")
        diff_html.write('<!--MajorDiffs-->\n')
        diff_html.close()
        timedout = True
        if tres is not None:
            # test program exited with error => expect major differences!
            ACCURACYerr = -1
        else:
            fs = open("%s%s.FILTERED" % (TST, STABLEerr))
            ft = open("%s.test.err.FILTERED" % TST)
            szs = os.fstat(fs.fileno())[6]
            szt = os.fstat(ft.fileno())[6]
            fs.close()
            ft.close()
            if szt < szs*0.5 or szt > szs*1.5:
                # filesizes differ significantly => expect major differences!
                ACCURACYerr = -1
        while timedout:
            cmd = ['Mdiff']
            if ACCURACYerr == -1:
                ACCURACYerr = 0
            else:
                cmd.append('-d')
            if not verbose:
                cmd.append('-q')
            cmd.extend(['-F^#', '-I%s' % par['IGNORE'],
                        '-C%s' % par['CONTEXT'], '-A%d' % ACCURACYerr,
                        titlefmt % ('out', 'out'),
                        '%s%s.FILTERED' % (TST, STABLEerr),
                        '%s.test.err.FILTERED' % TST,
                        '%s.err.diff.html' % TST])
            if procdebug:
                print 'RunTest: starting process "%s"\n' % '" "'.join(cmd)
            setpgrp = True
            proc = process.Popen(cmd, stdout = process.PIPE,
                                 stderr = process.PIPE)
            proc.killed = False
            t = Timer(float(par['TIMEOUT']), killProc, args = [proc])
            try:
                t.start()
                out, err = proc.communicate()
                t.cancel()
                if verbose or quiet:
                    if out:
                        STDOUT.write(out)
                    if err:
                        sys.stderr.write(err)
                if procdebug:
                    print 'RunTest: process exited "%s" (%s)\n' % \
                        ('" "'.join(cmd), proc.returncode)
            except KeyboardInterrupt:
                t.cancel()
                killProc(proc)
                if procdebug:
                    print 'RunTest: process killed "%s"\n' % '" "'.join(cmd)
                raise
            timedout = proc.killed
            ACCURACYerr = ACCURACYerr - 1
            if ACCURACYerr < 0:
                timedout = False # don't try again
        if env.get('ECHO_DIFF'):
            cmd = ['diff']
            if ACCURACYerr >= 0:
                cmd.append('-d')
            cmd.extend(['-Bb', '-F^#', '-I%s' % par['IGNORE'],
                        '-U%s' % par['CONTEXT'],
                        '%s%s.FILTERED' % (TST, STABLEerr),
                        '%s.test.err.FILTERED' % TST])
            proc = process.Popen(cmd)
            proc.wait()

        FailedOut, FailedErr = CategorizeResult(TST, max(sockerr, errcode))
        if FailedOut == F_OK and FailedErr == F_OK and testweb:
            for f in ['%s.out.diff.html' % TST, '%s.test.out' % TST,
                      '%s.server.out' % TST, '%s.client.out' % TST,
                      '%s.test.out.FILTERED' % TST,
                      '%s%s.FILTERED' % (TST, STABLEout),
                      '%s.err.diff.html' % TST, '%s.test.err' % TST,
                      '%s.server.err' % TST, '%s.client.err' % TST,
                      '%s.test.err.FILTERED' % TST,
                      '%s%s.FILTERED' % (TST, STABLEerr)]:
                remove(f)

        if not testweb:
            elem = AddTstToHtmlIndex(env, TST, STABLEout, STABLEerr, EXT,
                                     FailedOut, FailedErr)

        if not verbose and not quiet:
            if tres == 'socket':
                STDOUT.write("%sSOCKET%s" % (PURPLE, BLACK))
            elif tres == 'timeout':
                STDOUT.write("%sTIMEOUT%s" % (PURPLE, BLACK))
            elif tres == 'recursion':
                STDOUT.write("%sRECURSION%s" % (PURPLE, BLACK))
            elif tres == 'segfault':
                STDOUT.write("%sCRASHED%s" % (PURPLE, BLACK))
            elif tres == 'abort':
                STDOUT.write('%sABORTED%s' % (PURPLE, BLACK))
            elif tres == 'signal':
                STDOUT.write('%sSIGNALED%s' % (PURPLE, BLACK))
            else:
                if FailedOut == F_OK:
                    STDOUT.write('%sOK%s   ' % (GREEN, BLACK))
                elif FailedOut == F_WARN:
                    STDOUT.write('%sminor%s' % (GREEN, BLACK))
                else:
                    STDOUT.write('%sMAJOR%s' % (RED, BLACK))
                STDOUT.write(' ')
                if FailedErr == F_OK:
                    STDOUT.write('%sOK%s' % (GREEN, BLACK))
                elif FailedErr == F_WARN:
                    STDOUT.write('%sminor%s' % (GREEN, BLACK))
                else:
                    STDOUT.write('%sMAJOR%s' % (RED, BLACK))
            STDOUT.write('\n')

    return TX,FailedOut,FailedErr,elem,reason,links
### RunTest(env, TST, BusyPorts, COND, oktests) #

def CheckPort(port) :
    # Since 'localhost' and $HOST (i.e., `hostname`) are usually
    # different interfaces, we check both, unless $HOST (`hostname`)
    # appears to be merely an alias for 'localhost'.  That is, if the
    # hostname works, we prefer it over localhost, but don't require it,
    # such as e.g. on interwebless laptops.
    busy = 0
    Serrno = 0
    Serrstr = ""
    S0 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    S1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    S0.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    S1.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    host = os.environ['HOST']
    try:
        S1.bind((host,port))
    except socket.error, (Serrno,Serrstr):
        S1.close();
        host = 'localhost'
        try:
            S0.bind((host,port))
        except socket.error, (Serrno,Serrstr):
            S0.close()
            busy = 1

    return busy, host, Serrno, Serrstr, (S0, S1)
### CheckPort(port) #

def randomPort(l,h) :
    repeat = randomPortRepeat
    port = 0
    rpt = 0
    ports = []
    while rpt < repeat:
        port = random.randrange(l,h,1)
        ports.append(port)
        busy, host, Serrno, Serrstr, S = CheckPort(port)
        S[0].close()
        S[1].close()
        if busy:
            rpt = rpt + 1
            port = 0
        else:
            break
    return (port,host)
### randomPort(l,h) #

def CheckSocket2(env,SERVER) :  #,SrvrErr) :
    port = int(env[SERVER+'PORT'])
    newport = port
    busy, host, Serrno, Serrstr, S = CheckPort(port)
    if busy:
        S[0].close()
        S[1].close()
        Smsg = """
! Socket-Check failed for %sserver on <%s:%d> with #%d; '%s' !
""" % (SERVER, host, port, Serrno, Serrstr)
        newport = eval(dft[SERVER+'PORT'])
        if newport == 0:
            S[0].close()
            S[1].close()
            Smsg = Smsg + """
! Socket-Check failed for %sserver on <%s> !
! Giving up after %d attepts !
""" % (SERVER, host, randomPortRepeat)
            return None, Smsg

        env[SERVER+'PORT'] = newport
        os.environ[SERVER+'PORT'] = env[SERVER+'PORT']
        op = 'port=%d' % port
        np = 'port=%s' % env[SERVER+'PORT']
        env['exe']['Mserver']       = env['exe']['Mserver'][0]       , env['exe']['Mserver'][1].replace(op, np)
        env['exe']['MAL_Client']    = env['exe']['MAL_Client'][0]    , env['exe']['MAL_Client'][1].replace(op, np)
        env['exe']['SQL_Client']    = env['exe']['SQL_Client'][0]    , env['exe']['SQL_Client'][1].replace(op, np)
        env['exe']['SQL_Dump']      = env['exe']['SQL_Dump'][0]      , env['exe']['SQL_Dump'][1].replace(op, np)
        os.environ['MSERVER']       = os.environ['MSERVER'].replace(op, np)
        os.environ['MAL_CLIENT']    = os.environ['MAL_CLIENT'].replace(op, np)
        os.environ['SQL_CLIENT']    = os.environ['SQL_CLIENT'].replace(op, np)
        os.environ['SQL_DUMP']      = os.environ['SQL_DUMP'].replace(op, np)
        Smsg = Smsg + """
! Using new %sPORT=%s !
""" % (SERVER, env[SERVER+'PORT'])
#        STDERR.write(Smsg)
#        STDERR.flush()
#        SrvrErr.write(Smsg)
#        SrvrErr.flush()

    return S, None
### CheckSocket2(env,SERVER)    #,SrvrErr) #

def CheckSocket3(env,SERVER,ErrFileName) :
    res = F_OK
    port = int(env[SERVER+'PORT'])
    busy, host, Serrno, Serrstr, S = CheckPort(port)
    S[0].close()
    S[1].close()
    if busy:
        res = F_SOCK
        Smsg = """
! Socket-Check failed for %sserver on <%s:%d> with #%d; '%s' !
! %sPORT was not properly released by Mserver/mserver5 !
""" % (SERVER, host, port, Serrno, Serrstr, SERVER)
        STDERR.write(Smsg)
        STDERR.flush()
        ErrFile = open(ErrFileName, 'a')
        ErrFile.write(Smsg)
        ErrFile.flush()
        ErrFile.close()
    return res
### CheckSocket3(env,SERVER,ErrFileName) #

def prompt() :
    return time.strftime('%H:%M:%S> ',time.localtime(time.time()))
### prompt() #

def Prompt(cmd) :
    if type(cmd) is type([]):
        cmd = '" "'.join(cmd)
    prmpt = time.strftime('\n# %H:%M:%S >  ',time.localtime(time.time()))
    return '%s%s"%s"%s\n\n' % (prmpt, prmpt, cmd, prmpt)
### Prompt(cmd) #

def killProc(proc, outfile = None, cmd = None):
    if outfile is not None and cmd is not None:
        if type(cmd) is type([]):
            cmd = ' '.join(cmd)
        try:
            outfile.write('\n!Mtimeout: Timeout: %s\n' % cmd)
        except ValueError:
            print 'cannot write timeout message',cmd
    if os.name == "nt":
        sym = ''
        if os.path.exists(r'c:\Program Files\Debugging Tools for Windows (x64)\cdb.exe'):
            cdb = r'c:\Program Files\Debugging Tools for Windows (x64)\cdb.exe'
            if os.path.exists(r'c:\Symbols'):
                sym = r'c:\Symbols;'
        elif os.path.exists(r'c:\Program Files\Debugging Tools for Windows (x86)\cdb.exe'):
            cdb = r'c:\Program Files\Debugging Tools for Windows (x86)\cdb.exe'
            if os.path.exists('c:\WINDOWS\Symbols'):
                sym = r'c:\WINDOWS\Symbols;'
        else:
            cdb = None
        if cdb:
            p = process.Popen([cdb, '-pv', '-p', str(proc.pid),
                               '-y', '%scache*;srv*http://msdl.microsoft.com/download/symbols' % sym, '-lines', '-c', '~*kP;!locks;q'],
                              stdout = process.PIPE)
            out, err = p.communicate()
        else:
            out = ''
    else:
        try:
            p = process.Popen(['pstack', str(proc.pid)], stdout = process.PIPE)
            out, err = p.communicate()
        except:
            out = ''
    if outfile is not None and out:
        try:
            outfile.write('\n%s\n' % out)
        except ValueError:
            print 'cannot write stack trace'
    proc.killed = True
    try:
        os.kill(-proc.pid, signal.SIGKILL)
    except AttributeError:
        if procdebug:
            print 'killProc: starting process "taskkill" "/F" "/T" "/PID" "%s"\n' % str(proc.pid)
        p = process.Popen(['taskkill','/F','/T','/PID',str(proc.pid)],
                          stdout = process.PIPE, stderr = process.PIPE)
        out, err = p.communicate()
        if procdebug:
            print 'killProc: process exited "taskkill" "/F" "/T" "/PID" "%s" (%s)\n' % (str(proc.pid), proc.returncode)
    except OSError:
        pass

def LaunchIt(cmd, TestInput, TestOut, TestErr, TimeOut, SrvrOut = None) :
    global setpgrp
    if not SrvrOut:
        SrvrOut = process.PIPE

    TestOut.write(Prompt(cmd))
    TestOut.flush()
    TestErr.write(Prompt(cmd))
    TestErr.flush()

    if procdebug:
        print 'LaunchIt: starting process "%s" (inpipe)\n' % '" "'.join(cmd)
    setpgrp = True
    proc = process.Popen(cmd, stdin = process.PIPE, stdout = SrvrOut,
                         stderr = TestErr, universal_newlines = True)
    # maybe buffer output as it comes to avoid deadlock
    if SrvrOut == process.PIPE:
        proc.stdout = process._BufferedPipe(proc.stdout)
    if TestErr == process.PIPE:
        proc.stderr = process._BufferedPipe(proc.stderr)
    proc.killed = False
    t = Timer(TimeOut, killProc, args = [proc, TestErr, cmd])
    t.start()

    if TestInput:
        try:
            proc.stdin.write(TestInput)
            proc.stdin.flush()
        except IOError, (IOerrNo, IOerrStr):
            Warn("Flushing input pipe in LaunchIt failed with #%d: '%s'." % (IOerrNo, IOerrStr))

    return proc, t
### LaunchIt(cmd, TestIn, TestOut, TestErr, TimeOut, SrvrOut) #

def CollectIt(pOut, TestOut) :
    if pOut:
        while True:
            buf = pOut.read(8192)
            if not buf:
                break
            TestOut.write(buf)
### CollectIt(pOut, pErr, TestOut, TestErr) #

def RunIt(cmd, TestIn, TestOut, TestErr, TimeOut) :
    global setpgrp
    if type(TestIn) is type(''):
        TestInput = TestIn
        TestIn = process.PIPE
    else:
        TestInput = None
    TestOut.write(Prompt(cmd))
    TestOut.flush()
    TestErr.write(Prompt(cmd))
    TestErr.flush()
    if procdebug:
        print 'RunIt: starting process "%s"\n' % '" "'.join(cmd)
    setpgrp = True
    proc = process.Popen(cmd, stdin = TestIn, stdout = TestOut, stderr = TestErr, universal_newlines = True)
    proc.killed = False
    t = Timer(TimeOut, killProc, args = [proc, TestErr, cmd])
    try:
        t.start()
        # since both stdout and stderr are redirected to files,
        # communicate will not return any useful data
        proc.communicate(input = TestInput)
        t.cancel()
        if procdebug:
            print 'RunIt: process exited "%s" (%s)\n' % ('" "'.join(cmd), proc.returncode)
    except KeyboardInterrupt:
        t.cancel()
        killProc(proc, TestErr, cmd)
        if procdebug:
            print 'RunIt: process killed "%s"\n' % '" "'.join(cmd)
        raise
    rc = returnCode(proc, TestErr)
    if rc == 'interrupt':
        raise KeyboardInterrupt
    return rc
### RunIt(cmd, TestIn, TestOut, TestErr) #

def Log() :
    time.strftime('%H:%M:%S> ',time.localtime(time.time()))
### Log() #

def mapi_ping(port,lang) :
    retry = 0
    wait = 1
    host = 'localhost'
    while retry < 3:
        retry += 1
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((host, port))
            flag = sock.recv(2)
            unpacked = struct.unpack( '<H', flag )[0]  # little endian short
            len = ( unpacked >> 1 )     # get length
            data = sock.recv(len)
            # we don't send
            return True
        except socket.error, (Serrno,Serrstr):
            pass
        time.sleep(wait)
    return False
### mapi_ping() #

def DoIt(env, SERVER, CALL, TST, EXT, PRELUDE, TestOutFile, TestErrFile, STIMEOUT, CTIMEOUT, TIMEOUT, ME, MAPIsockets, length, nomito) :
    ATJOB2 = ""
    STDERR.flush()
    if quiet:
        STDOUT.write(".")
    elif verbose:
        STDOUT.write('%s%s %s (<=%d,%d,%d) ...' %
                     (prompt(), os.path.join(env['TSTDIR'], TST + EXT),
                      PRELUDE and PRELUDE[0] or '', TIMEOUT, CTIMEOUT, STIMEOUT))
    else:
        if ttywidth > 0 and length + 10 + 21 > ttywidth:
            # 10 - length of prompt()
            # 21 - length of time plus result
            l = ttywidth - 10 - 21 - 1
            if len(TST) <= l:
                s = '%-*s ' % (l, TST)
            else:
                s = '%s...%s ' % (TST[:l/2 - 2], TST[l/2+1-l:])
        else:
            s = '%-*s ' % (length, TST)
        STDOUT.write('%s%s' % (prompt(), s))
        if isatty:
            s = '(<=%d,%d,%d)' % (TIMEOUT, CTIMEOUT, STIMEOUT)
            STDOUT.write(s + '\b' * len(s))

    try:
        STDOUT.flush()
    except IOError, (IOerrNo, IOerrStr):
        Warn("Flushing STDOUT in DoIt failed with #%d: '%s'." % (IOerrNo, IOerrStr))
    TSTDB = env['TSTDB']
    exe = env['exe']

    if os.path.isfile(TST+".conf"):
        LOCAL_CONF = ['--config=%s.conf' % TST ]
    elif os.path.isfile(os.path.join(env['TSTSRCDIR'],"All.conf")):
        LOCAL_CONF = ['--config=%s' % os.path.join(env['TSTSRCDIR'],"All.conf")]
    else:
        LOCAL_CONF = []

    # Release reserved sockets and run the actual test
    MAPIsockets[0].close()
    MAPIsockets[1].close()

    returncode = None
    pSrvrCode = None
    ServerReady = True
    pSrvr = None
    pSrvrTimer = None
    try:
        if SERVER in ["MAL", "SQL"]:
            SrvrOutFile = TST+".server.out"
            SrvrErrFile = TST+".server.err"
            SrvrOut = open(SrvrOutFile,"w")
            SrvrErr = open(SrvrErrFile,"w")
            ClntOutFile = TST+".client.out"
            ClntErrFile = TST+".client.err"
            ClntOut = open(ClntOutFile,"w")
            ClntErr = open(ClntErrFile,"w")
            PROLOGUE = []
            DBINIT = []
            if os.path.isfile(TST + ".prologue5"):
                PROLOGUE = [TST + ".prologue5"]
            if os.path.isfile(TST + ".dbinit5"):
                dbinit = open(TST + ".dbinit5").readline().strip()
                if dbinit:
                    DBINIT = ['--dbinit=%s' % dbinit]

            Srvr = splitcommand(exe['Mserver'][1]) + LOCAL_CONF
            if nomito:
                try:
                    Srvr.remove('--forcemito')
                except ValueError:
                    pass
            Srvr.append('--dbpath=%s' % os.path.join(env['GDK_DBFARM'], TSTDB))
            if env.get('MULTIFARM'):
                Srvr.append('--dbextra=%s' % os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'))
                shutil.rmtree(os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'),
                              ignore_errors = True)
                os.makedirs(os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'))
            lang=""

            if SERVER == "MAL":
                lang="mal"
                Srvr.extend(['--set', 'mal_listing=2'])
                Srvr.extend(DBINIT)
            if SERVER == "SQL":
                lang="sql"
                Srvr.extend(['--set', 'mal_listing=0'])
                Srvr.extend(DBINIT)
            Srvr.extend(PROLOGUE)

            # enable r integration in server
            if CONDITIONALS['HAVE_LIBR']:
                Srvr.extend(['--set', 'embedded_r=yes'])

            pSrvr, pSrvrTimer = LaunchIt(Srvr, '\nio.printf("\\nReady.\\n");\n', SrvrOut, SrvrErr, TIMEOUT)
            ln="dummy"
            while 0 < len(ln) and not ln.startswith('Ready.'):
                ln=pSrvr.stdout.readline()
                SrvrOut.write(ln)
                SrvrOut.flush()
            if not ln.startswith('Ready.'):
                # If not ready, it may be that there are far too many
                # network connections in use, all in TIME_WAIT status.
                # We'll just wait a while for that to clear and try
                # again.
                pSrvr.stdin.close()
                CollectIt(pSrvr.stdout, SrvrOut)
                pSrvr.wait()
                pSrvrTimer.cancel()
                if returnCode(pSrvr) == 'error':
                    time.sleep(120)
                    pSrvr, pSrvrTimer = LaunchIt(Srvr, '\nio.printf("\\nReady.\\n");\n', SrvrOut, SrvrErr, TIMEOUT)
                    ln="dummy"
                    while 0 < len(ln) and not ln.startswith('Ready.'):
                        ln=pSrvr.stdout.readline()
                        SrvrOut.write(ln)
                        SrvrOut.flush()
                if not ln.startswith('Ready.'):
                    ServerReady = False

            if ServerReady:
                port = int(env['MAPIPORT'])
                ServerReady = mapi_ping(port, lang)
                if not ServerReady:
                    pSrvr.stdin.close()
                    CollectIt(pSrvr.stdout, SrvrOut)
                    pSrvr.wait()
                    pSrvrTimer.cancel()
                    if returnCode(pSrvr) == 'error':
                        time.sleep(120)
                        ServerReady = True
                        pSrvr, pSrvrTimer = LaunchIt(Srvr, '\nio.printf("\\nReady.\\n");\n', SrvrOut, SrvrErr, TIMEOUT)
                        ln="dummy"
                        while 0 < len(ln) and not ln.startswith('Ready.'):
                            ln=pSrvr.stdout.readline()
                            SrvrOut.write(ln)
                            SrvrOut.flush()
                    if not ln.startswith('Ready.'):
                        ServerReady = False

                    if ServerReady:
                        port = int(env['MAPIPORT'])
                        ServerReady = mapi_ping(port, lang)

        else:
            ClntOut = open(TestOutFile, 'a')
            ClntErr = open(TestErrFile, 'a')

        if ServerReady:
            if   CALL == "other":
                cmd = [os.path.join(".", TST + EXT), TST] + PRELUDE
                returncode = RunIt(cmd, "", ClntOut, ClntErr, CTIMEOUT)
            elif CALL == "python":
                cmd = splitcommand(exe['python'][1]) + [TST + EXT, TST] + PRELUDE
                returncode = RunIt(cmd, "", ClntOut, ClntErr, CTIMEOUT)
            elif CALL in ["mal", "malXs"]:
                cmd = splitcommand(exe['Mserver'][1]) + LOCAL_CONF + PRELUDE
                cmd.append('--dbpath=%s' % os.path.join(env['GDK_DBFARM'], TSTDB))
                if env.get('MULTIFARM'):
                    cmd.append('--dbextra=%s' % os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'))
                    shutil.rmtree(os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'),
                                  ignore_errors = True)
                    os.makedirs(os.path.join(env['GDK_DBFARM'], TSTDB + '_transient'))
                if CALL == "mal":
                    X=""
                else:
                    X="_s[0-9][0-9]"
                test = re.compile("^"+TST+X+EXT+"$", re.MULTILINE)
                d = os.listdir(os.getcwd())
                d.sort()
                for f in d:
                    if test.match(f):
                        cmd.append(f)
                        returncode = RunIt(cmd, open(os.devnull), ClntOut, ClntErr, TIMEOUT)
                    if returncode:
                        break
            elif CALL in ["malC", "malCXs"]:
                TSTs = []
                if CALL == "malC":
                    X=""
                else:
                    X="_s[0-9][0-9]"
                test = re.compile("^"+TST+X+EXT+"$", re.MULTILINE)
                d = os.listdir(os.getcwd())
                d.sort()
                for f in d:
                    if test.match(f):
                        TSTs.append(f)

                if CALL.startswith("mal"):
                    Clnt = splitcommand(exe['MAL_Client'][1])
                else:
                    Clnt = []   # cannot happen
                for f in TSTs:
                    returncode = RunIt(Clnt, open(f), ClntOut, ClntErr, TIMEOUT)
                    if returncode:
                        break

            elif CALL in ["sql", "sqlXs"]:
                TSTs = []
                if CALL == "sql":
                    X=""
                else:
                    X="_s[0-9][0-9]"
                test = re.compile("^"+TST+X+EXT+"$", re.MULTILINE)
                d = os.listdir(os.getcwd())
                d.sort()
                for f in d:
                    if test.match(f):
                        TSTs.append(f)

                Clnt = splitcommand(exe['SQL_Client'][1])
                for f in TSTs:
                    returncode = RunIt(Clnt, open(f), ClntOut, ClntErr, TIMEOUT)
                    if returncode:
                        break
            elif CALL == "R":
                Clnt = splitcommand(exe['R_Client'][1])
                RunIt(Clnt, open(TST+EXT), ClntOut, ClntErr, TIMEOUT)
        else:
            for fp in ClntOut,ClntErr:
                fp.write('\n\n! Server not ready; skipping attempt to start client!\n\n')
        ClntOut.close()
        ClntErr.close()

        if SERVER in ["MAL", "SQL"]:
            EPILOGUE = None
            if os.path.isfile(TST+".epilogue5"):
                EPILOGUE = open(TST+".epilogue5",'r')
            if EPILOGUE:
                EpiFailed = ""
                try:
                    pSrvr.stdin.write(EPILOGUE.read())
                    pSrvr.stdin.flush()
                    pSrvr.stdin.write(';\nio.printf("\\nDone..\\n");\n')
                    pSrvr.stdin.flush()
                except IOError, (IOerrNo, IOerrStr):
                    EpiFailed = EpiFailed+"\n! Executing "+TST+".epilogue failed with #"+str(IOerrNo)+": '"+IOerrStr+"'. !"
                    EpiFailed = EpiFailed+"\n! Probably, Mserver/mserver5 has died before or during. !\n"
                ln="dummy"
                while 0 < len(ln) and not ln.startswith('Done..'):
                    ln=pSrvr.stdout.readline()
                    SrvrOut.write(ln)
                    SrvrOut.flush()
                SrvrOut.write(EpiFailed)
                SrvrOut.flush()
                EPILOGUE.close()

            if ServerReady:
                try:
                    pSrvr.stdin.write('clients.quit();\n')
                    pSrvr.stdin.flush()
                except IOError:
                    pass
                try:
                    pSrvr.stdin.close()
                except IOError, (IOerrNo, IOerrStr):
                    Warn("Closing input pipe in DoIt failed with #%d: '%s'." % (IOerrNo, IOerrStr))

            CollectIt(pSrvr.stdout, SrvrOut)
            pSrvr.wait()
            pSrvrTimer.cancel()
            if procdebug:
                print 'DoIt: process exited "%s" (%s)\n' % ('" "'.join(Srvr), pSrvr.returncode)
            pSrvrTimer = None
            pSrvrCode = returnCode(pSrvr, SrvrErr)

            AllOut = [SrvrOut, ClntOutFile]
            AllErr = [SrvrErr, ClntErrFile]
            TestOut = open(TestOutFile, 'a')
            for q in AllOut:
                if type(q) is type(''):
                    n = q
                else:
                    n = q.name
                    q.close()
                q = open(n,'r')
                try:
                    TestOut.write(q.read())
                except IOError, (IOerrNo, IOerrStr):
                    Warn("Reading from input '%s' or writing to output '%s' failed with #%d: '%s'." % (q.name, TestOut.name, IOerrNo, IOerrStr))
                except MemoryError:
                    Warn("Reading from input '%s' or writing to output '%s' failed with 'MemoryError'." % (q.name, TestOut.name))
                TestOut.flush()
                q.close()
            TestErr = open(TestErrFile, 'a')
            for q in AllErr:
                if type(q) is type(''):
                    n = q
                else:
                    n = q.name
                    q.close()
                q = open(n,'r')
                TestErr.write(q.read())
                TestErr.flush()
                q.close()
        else:
            TestOut = try_open(TestOutFile, 'a')
            TestErr = try_open(TestErrFile, 'a')

        if TestOut is not None:
            TestOut.write(Prompt('Done.'))
            TestOut.close()
        if TestErr is not None:
            TestErr.write(Prompt('Done.'))
            TestErr.close()
        # there are two tests that create a temporary file called
        # /tmp/MonetEvents; we just unconditionally remove the file
        remove('/tmp/MonetEvents')

    except KeyboardInterrupt:
        if pSrvrTimer is not None:
            pSrvrTimer.cancel()
            killProc(pSrvr, SrvrErr, Srvr)
        raise

    if returncode is not None or pSrvrCode is not None:
        # something failed
        if returncode == 'interrupt' or pSrvrCode == 'interrupt':
            raise KeyboardInterrupt
        for err in ('timeout', 'segfault', 'abort', 'signal', 'error'):
            if pSrvrCode == err or returncode == err:
                return err
        return returncode or pSrvrCode # remaining error (shouldn't get here)

    if CALL not in ('python', 'other'):
        # running mserver/mclient directly, so we know they didn't fail
        return None

    # Try to detect segfaults and the like
    # Try to detect aborts due to too deep recursion
    for (regexp, msg) in [("(^(|[^#]*[\t ])((Memory|Segmentation) [Ff]ault|Bus [Ee]rror|Aborted|Assertion (|.* )failed[:\.]|!FATAL: BATSIGabort:)([ \t]|$))",
                           'segfault'),
                          ("aborted too deep recursion",
                           'recursion'),
                          ("mal_mapi\.listen:operation failed: bind to stream socket port",
                           'socket')]:
        TO = re.compile(regexp, re.MULTILINE)
        # FIXME: this begs for a much nicer solution (100% copy of below)
        for f in (TestErrFile, TestOutFile):
            if os.path.isfile(f):
                for l in open(f):
                    if TO.search(l):
                        return msg

    return None

### DoIt(env, SERVER, CALL, TST, EXT, PRELUDE, TestOutFile, TestErrFile, STIMEOUT, CTIMEOUT, TIMEOUT, ME, MAPIsockets, length, nomito) #

def Check(command, input) :
    global setpgrp
    if procdebug:
        print 'Check: starting process "%s" (inpipe,outpipe,errpipe)\n' % '" "'.join(command)
    setpgrp = True
    proc = process.Popen(command, stdin = process.PIPE, stdout = process.PIPE,
                         stderr = process.PIPE, universal_newlines = True)
    proc.killed = False
    t = Timer(float(par['TIMEOUT']), killProc, args = [proc])
    try:
        t.start()
        qOut, qErr = proc.communicate(input = input)
        t.cancel()
        if procdebug:
            print 'Check: process exited "%s" (%s)\n' % ('" "'.join(command), proc.returncode)
    except KeyboardInterrupt:
        t.cancel()
        killProc(proc)
        if procdebug:
            print 'Check: process killed "%s"\n' % '" "'.join(command)
        raise
    qOut = qOut.split('\n')
    qErr = qErr.split('\n')
    if proc.returncode:
        qOut.append('! Exit 1')
    test = re.compile( r"^!WARNING: BATpropcheck: "                                          "|"
                       r"^!WARNING: monet_checkbat: "                                        "|"
                       r"^!WARNING: GDKlockHome: ignoring empty or invalid .gdk_lock."       "|"
                       r"^!WARNING: BBPdir: initializing BBP.",
                       re.MULTILINE)
    noErr = []
    for l in qOut+qErr:
        if l.startswith("!"):
            if test.match(l):
                if not l.startswith("!WARNING: "):
                    noErr.append(l+"\n")
            else:
                ErrMsg('"%s" failed:' % '" "'.join(command))
                if qOut and qOut[-1].startswith("! Exit 1"):
                    qErr.append(qOut.pop())
                for l in qOut+qErr:
                    STDERR.write(l)
                    STDERR.write("\n")
                STDERR.write("\n")
                STDERR.flush()
                #sys.exit(1)
                if sys.platform == 'linux2':
                    proc = process.Popen(['netstat', '-ap'],
                                         stdout = process.PIPE,
                                         stderr = process.PIPE,
                                         universal_newlines = True)
                    out, err = proc.communicate()
                    STDERR.write(err)
                    STDOUT.write(out)
    if noErr:
        STDOUT.flush()
        STDERR.writelines(noErr)
        STDERR.flush()
    return 0
### Check(command, input) #

def CheckClassPath() :
    if os.environ.has_key('CLASSPATH'):
        cp = os.environ['CLASSPATH']
        cpx = cp + os.pathsep
    else:
        cp = ''
        cpx = ''
    JARS = {
        'HAVE_MONETDBJDBC_JAR' : re.compile('^monetdb-jdbc-[0-9]\.[0-9]+(-[a-f0-9]{12})?\.jar$'),
        'HAVE_JDBCCLIENT_JAR'  : re.compile('^jdbcclient\.jar$'),
        'HAVE_JDBCTESTS_JAR'   : re.compile('^jdbctests\.jar$'),
    }
    # check for known JARs in CLASSPATH files
    for p in cp.split(os.pathsep):
        if os.path.isfile(p):
            f = os.path.basename(p)
            C = 'HAVE_%s' % f.upper().replace('.','_')
            if not JARS.has_key(C):
                C = 'HAVE_MONETDBJDBC_JAR'
            if JARS[C].match(f):
                CONDITIONALS[C] = '#'
    # check for known JARs in CLASSPATH directories
    # + fall-back using pkgdatadir/lib
    cpx += _configure(os.path.join('${prefix}/share','monetdb','lib'))
    for d in cpx.split(os.pathsep):
        if os.path.isdir(d):
            for f in os.listdir(d):
                p = os.path.join(d,f)
                if os.path.isfile(p):
                    if f == 'BugConcurrent_clients_SF_1504657.class':
                        C = 'HAVE_JDBCTESTS_DIR'
                        if not CONDITIONALS.get(C):
                            cp = cp + os.pathsep + d
                            CONDITIONALS[C] = '#'
                    else:
                        C = 'HAVE_%s' % f.upper().replace('.','_')
                        if not JARS.has_key(C):
                            C = 'HAVE_MONETDBJDBC_JAR'
                        if not CONDITIONALS.get(C) and JARS[C].match(f):
                            cp = cp + os.pathsep + p
                            CONDITIONALS[C] = '#'
    if cp:
        os.environ['CLASSPATH'] = cp
    if verbose:
        miss = ''
        for j in ['monetdbjdbc.jar', 'jdbcclient.jar', 'jdbctests.jar']:
            C = 'HAVE_%s' % j.upper().replace('.','_')
            if not CONDITIONALS.get(C):
                miss += ' "%s"' % j
        if miss:
            Warn('Could not find%s in\nCLASSPATH="%s"' % (miss,cpx))
    if CONDITIONALS.get('HAVE_MONETDBJDBC_JAR') and \
       ( CONDITIONALS.get('HAVE_JDBCTESTS_JAR') or
         CONDITIONALS.get('HAVE_JDBCTESTS_DIR') ):
        CONDITIONALS['HAVE_JDBCTESTS'] = '#'
### CheckClassPath() #

def SetExecEnv(exe,verbose) :
    if os.name == "nt":
        CALL = "call "
    else:
        CALL = ""
    if verbose:
        STDERR.flush()
    for v in exe.keys():
        V = v.upper()
        if  v != 'Mtimeout':
            os.environ[V] = CALL+exe['Mtimeout'][1]+exe[v][1]
        elif exe[v][0]:
            os.environ[V] = CALL+exe[v][1]
        else:
            os.environ[V] = ""
        if verbose:
            print "%s = %s : %s" % (V, exe[v][0], exe[v][1])
    if verbose:
        STDOUT.flush()
### SetExecEnv(exe,procdebug) #

def ReadMapproveRc(f) :
    v = {}
    v['SYST'] = SYST
    v['RELEASE'] = RELEASE
    v['DIST'] = DIST
    v['VERSION'] = VERSION
    v['BITS'] = ''
    v['OIDS'] = ''
    v['INT128'] = ''
    v['SINGLE'] = ''
    v['STATIC'] = ''
    if os.path.isfile(f):
        r = re.compile('^([A-Z][A-Z0-9_]*) = "(.*)".*$')
        for l in open(f):
            m = r.match(l)
            if m:
                v[m.group(1)] = m.group(2)
    return v
### ReadMapproveRc(f) #

#############################################################################
#       MAIN

THISFILE = os.path.basename(sys.argv[0])
THISPATH = os.path.realpath(os.path.dirname(sys.argv[0]))
dftIGNORE = '^#'
TSTDBG = str(2+8)
TSTTHREADS = "0"
dftTSTPREF = "mTests"
TSTSUFF = "Tests"

if hasattr(os,"symlink"):
    SymlinkOrCopy = os.symlink
else:
    def SymlinkOrCopy(src, dst):
        shutil.copy(os.path.normpath(os.path.join(os.getcwd(), src)), dst)

os.environ['CYGPATH_W'] = 'echo'
os.environ['CYGPATH_WP'] = 'echo'

HOST = 'localhost'
if os.environ.has_key('HOST'):
    HOST = os.environ['HOST']
#else:
#    HOST = ''
elif os.name != "nt":
    HOST = os.uname()[1]
elif os.environ.has_key('COMPUTERNAME'):
    HOST = os.environ['COMPUTERNAME']
##else:
##    HOST = "WIN2000"
if os.environ.has_key('DOMAIN'):
    HOST = HOST.replace('.'+os.environ('DOMAIN'),'')
else:
    HOST = HOST.split('.', 1)[0]
os.environ['HOST'] = HOST
# check the host port actually works
_, HOST = randomPort(30000,39999)
os.environ['HOST'] = HOST
os.environ['MAPIHOST'] = HOST

if os.name == "nt":
    SYST    = "Windows"
    RELEASE = "5.0"
    r = re.compile('^Microsoft Windows (.*)\[Version ([0-9]+\.[0-9]+)([^\[0-9].*)\]$')
    if procdebug:
        print 'starting process "cmd" "/c" "ver" (inpipe,outpipe)\n'
    proc = process.Popen('cmd /c ver', stdin = process.PIPE,
                         stdout = process.PIPE, stderr = process.PIPE,
                         universal_newlines = True)
    qOut, qErr = proc.communicate()
    if procdebug:
        print 'process exited "cmd" "/c" "ver" (%s)\n' % proc.returncode
    for l in qOut.split('\n'):
        m = r.match(l.strip())
        if m and m.group(2):
            RELEASE = m.group(2)
else:
    SYST    = os.uname()[0].split("_NT-", 1)[0].replace("-","")
    if SYST == "AIX":
        RELEASE = os.uname()[3]+"."+os.uname()[2]
    else:
        RELEASE = os.uname()[2].split("(", 1)[0]
        MAJOR = RELEASE.split(".", 1)[0]
        if "A" <= MAJOR and MAJOR <= "Z":
            RELEASE = RELEASE.split(".", 1)[1]

# this is for the wine/mingw setup
if sys.platform == 'linux2' and CONDITIONALS['CROSS_COMPILING']:
    SYST = 'Windows'
    HOST = "WINE"
    RELEASE = "5.2"

# see if we can use UNIX sockets
try:
    server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
except (socket.error, AttributeError):
    # apparently not
    SOCK = False
else:
    SOCK = True

if SYST == "Linux":
    #  Please keep this aligned / in sync with configure.ag !
    LINUX_DIST=''
    if os.path.isfile('/etc/os-release'):
        l = open('/etc/os-release').read()
        x0 = re.search('^NAME=[\'"]?([^\'"\n]*)[\'"]?$', l, re.MULTILINE)
        if x0:
            x0 = x0.group(1)
        x1 = re.search('^VERSION_ID=[\'"]?([^\'"\n]*)[\'"]?$', l, re.MULTILINE)
        if x1:
            x1 = x1.group(1)
        LINUX_DIST = '%s:%s' % (x0 or 'Linux', x1 or '')
    elif os.path.isfile('/etc/fedora-release'):
        l = open('/etc/fedora-release').readline()
        x = re.match('^.*(Fedora).* release ([0-9][^ \n]*)( .*)*$', l)
        if x:
            LINUX_DIST = '%s:%s' % (x.group(1),x.group(2))
    elif os.path.isfile('/etc/centos-release'):
        l = open('/etc/centos-release').readline()
        x = re.match('^(CentOS).* release ([0-9][^ \n]*)( .*)*$', l)
        if x:
            LINUX_DIST = '%s:%s' % (x.group(1),x.group(2))
    elif os.path.isfile('/etc/yellowdog-release'):
        l = open('/etc/yellowdog-release').readline()
        x = re.match('^(Yellow) Dog Linux release ([0-9][^ \n]*)( .*)*$', l)
        if x:
            LINUX_DIST = '%s:%s' % (x.group(1),x.group(2))
    elif os.path.isfile('/etc/redhat-release'):
        l = open('/etc/redhat-release').readline()
        x0 = re.match('^.*(Red) (Hat).* Linux *([A-Z]*) release ([0-9][^ \n]*)( .*)*$', l)
        x1 = re.match('^Red Hat Enterprise Linux ([AW]S) release ([0-9][^ \n]*)( .*)*$', l)
        x2 = re.match('^(CentOS).* release ([0-9][^ \n]*)( .*)*$', l)
        x3 = re.match('^(Scientific) Linux SL release ([0-9][^ \n]*)( .*)*$', l)
        if x0:
            LINUX_DIST = '%s%s:%s%s' % (x0.group(1),x0.group(2),x0.group(4),x0.group(3))
        elif x1:
            LINUX_DIST = 'RHEL:%s%s' % (x1.group(2),x1.group(1))
        elif x2:
            LINUX_DIST = '%s:%s' % (x2.group(1),x2.group(2))
        elif x3:
            LINUX_DIST = '%s:%s' % (x3.group(1),x3.group(2))
    elif os.path.isfile('/etc/SuSE-release'):
        l = open('/etc/SuSE-release').readline()
        x0 = re.match('^.*(S[Uu]SE) LINUX Enterprise ([SD])[ervsktop]* ([0-9][^ \n]*)( .*)*$', l)
        x1 = re.match('^S[Uu]SE LINUX Enterprise ([SD])[ervsktop]* ([0-9][^ \n]*)( .*)*$', l)
        x2 = re.match('^.*(S[Uu]SE) [Ll][Ii][Nn][Uu][Xx].* ([0-9][^ \n]*)( .*)*$', l)
        x3 = re.match('^open(S[Uu]SE) ([0-9][^ \n]*)( .*)*$', l)
        if x0:
            LINUX_DIST = '%s:%sE%s' % (x0.group(1),x0.group(3),x0.group(2))
        elif x1:
            LINUX_DIST = 'SLE%s:%s' % (x1.group(1),x1.group(2))
        elif x2:
            LINUX_DIST = '%s:%s' % (x2.group(1),x2.group(2))
        elif x3:
            LINUX_DIST = '%s:%s' % (x3.group(1),x3.group(2))
    elif os.path.isfile('/etc/gentoo-release'):
        l = open('/etc/gentoo-release').readline()
        x = re.match('^.*(Gentoo) Base System.* [versionrelease]* ([0-9][^ \n]*)( .*)*$', l)
        if x:
            LINUX_DIST = '%s:%s' % (x.group(1),x.group(2))
    elif os.path.isfile('/etc/lsb-release'):
        x0 = x1 = None
        for l in open('/etc/lsb-release'):
            if not x0:
                x0 = re.match('^DISTRIB_ID=([^ \n]*)( .*)*$', l)
            if not x1:
                x1 = re.match('^DISTRIB_RELEASE=([^ \n]*)( .*)*$', l)
        if x0 and x1:
            LINUX_DIST = '%s:%s' % (x0.group(1),x1.group(1))
    elif os.path.isfile('/etc/debian_version'):
        LINUX_DIST = "Debian:"+open('/etc/debian_version').readline().strip()
    if not LINUX_DIST:
        LINUX_DIST = SYST+':'+re.match('^([0-9\.]*)([^0-9\.].*)$', RELEASE).group(1)
    DIST,VERSION = LINUX_DIST.split(':', 1)
elif SYST == "SunOS" and os.path.isfile('/etc/release'):
    (DIST,VERSION,rest) = open('/etc/release').readline().strip().split(' ',2)
else:
    DIST = SYST
    VERSION = RELEASE

SYSTVER = SYST+RELEASE
DISTVER = DIST+VERSION
os.environ['SYST'] = SYST
os.environ['SYSTVER'] = SYSTVER
os.environ['RELEASE'] = RELEASE
os.environ['DIST'] = DIST
os.environ['DISTVER'] = DISTVER
os.environ['VERSION'] = VERSION

if os.environ.has_key('COMPBITSOIDSLINK'):
    COMPBITSOIDSLINK = os.environ['COMPBITSOIDSLINK']
else:
    COMPBITSOIDSLINK = ""

if os.environ.has_key('HTMLTITLE'):
    HTMLTITLE = os.environ['HTMLTITLE']
else:
    HTMLTITLE = ""
    if COMPBITSOIDSLINK:
        HTMLTITLE = " for "+COMPBITSOIDSLINK
    HTMLTITLE = ""+THISFILE+" results"+HTMLTITLE+" on "+DISTVER       #"+ ("`date`")"

URLPREFIX = 'http://dev.monetdb.org/hg/MonetDB/file/'

par = {}
dft = {}

def main(argv) :
    #TODO:
    #signal.signal(signal.SIGALRM, AlarmHandler)

    vars = ['TSTSRCBASE', 'TSTTRGBASE']
    if THISFILE == "Mtest.py":
        vars = vars + [ 'MALCLIENT', 'SQLCLIENT', 'SQLDUMP', 'RCLIENT']    #, 'MONETDB_MOD_PATH' ]

    env = {}
    env['DIRSEP'] = os.sep

    # most intuitive (?) default settings
    dft['TSTSRCBASE']     = r"_configure('/home/release/release/MonetDB')"
    dft['TSTTRGBASE']     = r"_configure('/usr/local')"   # or os.getcwd() ?
    if THISFILE == "Mtest.py":
        dft['GDK_DEBUG']      = "TSTDBG"
        dft['GDK_NR_THREADS'] = "TSTTHREADS"
        dft['MONETDB_MOD_PATH'] = "''"
        dft['setMONETDB_MOD_PATH'] = "'--set \"monet_mod_path='+env['MONETDB_MOD_PATH']+'\"'"
        dft['MAPIPORT']       = "str(randomPort(30000,39999)[0])"
        dft['MALCLIENT']      = "'mclient -lmal -ftest -Eutf-8'"
        dft['SQLCLIENT']      = "'mclient -lsql -ftest -Eutf-8'"
        dft['SQLDUMP']        = "'msqldump -q'"
        dft['RCLIENT']        = "'R --vanilla --slave'"

    #par = {}
    # get current environment
    env['HOST'] = os.environ['HOST']
    for v in vars:
        if os.environ.has_key(v):
            env[v] = os.environ[v]
            #TODO:
            # make sure, that PATHs are absolute

    # commandline options overrule environment
    cmd_options = [
        # long name, short name, GDK option, argument, comment
        ('recursive', 'r', 'recursive', None,
         "recurse into subdirectories (implies 'All')"),
        ('revision', None, 'revision', '<hgid>',
         'use given revision as the HG short hash'),
        ('TSTSRCBASE', None, 'TSTSRCBASE', '<path>',
         'default: "%s"' % _configure('/home/release/release/MonetDB')),
        ('TSTTRGBASE', None, 'TSTTRGBASE', '<path>',
         'default: "%s"' % _configure('/usr/local')),
        ('quiet', 'q', 'quiet', None,
         "suppress messages on stdout"),
        ('verbose', 'v', 'verbose', None,
         "more verbose test output"),
        ('procdebug', None, 'procdebug', None,
         'process debugging (Mtest developers only)'),
        ]

    if THISFILE == "Mtest.py":
        common_options = cmd_options + [
            (None, 'I', 'ignore', '<exp>',
             "ignore lines matching <exp> during diff (default: '%s')" % dftIGNORE),
            (None, 'C', 'context', '<num>',
             "use <num> lines of context during diff (default: -C1)"),
            (None, 'A', 'accuracy', '<num>',
             "accuracy for diff: 0=lines, 1=words, 2=chars (default: -A1)"),
            (None, 't', 'timeout', '<sec>',
             "timeout: kill (hanging) tests after <sec> seconds;\n"
             "-t0 means no timeout (default: -t60)"),
            ('debug', 'd', 'debug', '<num>',
             ("debug value to be used by mserver5 (default: -d%s)\n"
              "(see `mserver5 --help' for details)") % TSTDBG),
            ('nr_threads', 'n', 'nr_threads', '<num>',
             ("number of threads for mserver5 (default: -n%s)\n"
              "-n0 => mserver5 automatically determines the number of CPU cores") % TSTTHREADS),
            ('monet_mod_path', None, 'monet_mod_path', '<pathlist>',
             "override mserver5's default module search path"),
            ('dbfarm', None, 'gdk_dbfarm', '<directory>',
             "override default location of database directory"),
            ('MALCLIENT', None, 'MALCLIENT', '<mal-client program>',
             'default: %s' % dft['MALCLIENT']),
            ('SQLCLIENT', None, 'SQLCLIENT', '<sql-client program>',
             'default: %s' % dft['SQLCLIENT']),
            ('SQLDUMP', None, 'SQLDUMP', '<sql-dump program>',
             'default: %s' % dft['SQLDUMP']),
            ('RCLIENT', None, 'RCLIENT', '<R program>',
             'default: %s' % dft['RCLIENT']),
            ('concurrent', None, 'concurrent', None,
             'There are concurrent Mtest runs using the same MonetDB installation'),
            ('dbg', None, 'dbg', '<debugger/valgrind>',
             "debugger to start before each server"),
            ('echo-diff', None, 'echo-diff', None,
             "echo differences between stable and current test output to console (stdout)"),
            ('mserver_set', None, 'mserver_set', '<Mserver_option>',
             "This passes a single set to the server"),
            ('no-clean', None, 'no_clean', None, 'Do not clean up before test'),
            ('testweb', None, 'testweb', None, 'Optimize testing for testweb'),
            ('releaserun', None, 'releaserun', None,
             'run tests as if for a release test'),
            ('multifarm', None, 'multifarm', None,
             'use multiple dbfarms (developers only)'),
            ('nomito', None, 'nomito', None,
             'Do not pass --forcemito to server'),
            ('jenkins', None, 'jenkins', None,
             'special handling for Jenkins'),
            ('addreqs', None, 'addreqs', None,
             'automatically add required tests when testing individual tests'),
            ]

    if THISFILE == 'Mtest.py':
        options = common_options # + []
    elif THISFILE == 'Mapprove.py':
        f = _configure(os.path.join('/usr/local',dftTSTPREF,'.Mapprove.rc'))
        v = ReadMapproveRc(f)
        for i in 'BITS', 'OIDS', 'INT128', 'SINGLE', 'STATIC':
            if v[i]:
                v[i] = '[.%s]' % v[i]
        options = cmd_options + [
            (None, 'x', 'ext', '<ext>',
             "approve only output files *.<ext><sys> (<ext> = 'out' or 'err')\n"
             "(default: <ext> = 'out' & 'err')"),
            (None, 'S', 'sys', '<sys>',
             "approve specific output *.<ext><sys>\n"
             "(<sys> = '[.(<SYST>[<RELEASE>]|<DIST>[<VERSION>])][.(32|64)bit][.oid(32|64)][.int128][.single][.STATIC]',\n"
             "(default: longest match for <sys> = '[.(%s[%s]|%s[%s])]%s%s%s%s%s')"
              % (v['SYST'], v['RELEASE'], v['DIST'], v['VERSION'], v['BITS'], v['OIDS'], v['INT128'], v['SINGLE'], v['STATIC'])),
            (None, 'f', 'force', None,
             "force approval of error messages (i.e., lines starting with '!')"),
            ('nopatch', None, 'nopatch', None,
             "do not attempt to patch other outputs"),
            ]
    else:
        options = []

    try:
        # let monet_options.parse_options() parse the command line arguments
        # without setting a --config default in case the is no --config given
        opts, args = monet_options.parse_options(argv[1:], options, Usage, False)
    except monet_options.Error:
        sys.exit(1)

    if len(args) == 1 and not os.path.isdir(args[0]):
        head, tail = os.path.split(args[0])
        if os.path.isfile(args[0]):
            head, tst = os.path.split(head)
            if tst != 'Tests':
                ErrXit("%s: not a valid test name" % args[0])
            args = [head]
            if tail != 'All':
                for ext in ('_s00.malC', '_p00.malC', '_s00.mal', '_s00.sql',
                            '_p00.sql', '.MAL.py', '.SQL.py', '.malC', '.mal',
                            '.sql', '.py', '.R', ''):
                    # extentions .in and .src are never combined
                    if tail.endswith(ext + '.in'):
                        args.append(tail[:-len(ext + '.in')])
                        break
                    if tail.endswith(ext + '.src'):
                        args.append(tail[:-len(ext + '.src')])
                        break
                    if tail.endswith(ext):
                        args.append(tail[:-len(ext)])
                        break
                else:
                    ErrXit("%s: not a valid test name" % args[0])
        elif head and tail and os.path.isdir(head) and tail != 'Tests' and os.path.isdir(os.path.join(head, 'Tests')):
            args = [head, tail]

    config = opts.get('config', '')
    if config:
        config = ' "--config=%s"' % config

    recursive = opts.get('recursive', False)
    global testweb
    testweb = opts.get('testweb', False)
    global quiet
    quiet = opts.get('quiet', False)
    global verbose
    verbose = opts.get('verbose', False)
    if quiet and verbose:
        ErrExit('--verbose and --quiet are mutually exclusive')
    global procdebug
    procdebug = opts.get('procdebug', False)
    global releaserun
    releaserun = opts.get('releaserun', releaserun)
    nomito = opts.get('nomito', False)
    CONDITIONALS['RELEASERUN'] = releaserun
    jenkins = False
    addreqs = False
    if THISFILE == "Mtest.py":
        _IGNORE = dftIGNORE
        par['IGNORE'] = opts.get('ignore', _IGNORE)
        par['CONTEXT'] = opts.get('context', '1')
        a = int(opts.get('accuracy', 1))
        if a not in (-1,0,1,2):
            ErrExit('Accuracy for diff (-A) must be one of: 0=lines, 1=words, 2=chars !')
        par['ACCURACY'] = a
        par['TIMEOUT'] = int(opts.get('timeout', 60))
        a = opts.get('debug')
        if a is not None:
            env['GDK_DEBUG'] = str(int(a))
        a = opts.get('nr_threads')
        if a is not None:
            env['GDK_NR_THREADS'] = str(int(a))
        a = opts.get('monet_mod_path')
        if a is not None:
            env['MONETDB_MOD_PATH'] = a
        a = opts.get('gdk_dbfarm')
        if a is not None:
            env['GDK_DBFARM'] = a
        a = opts.get('concurrent')
        if a is not None:
            env['CONCURRENT'] = a
        a = opts.get('dbg')
        if a is not None:
            env['DBG'] = a
        a = opts.get('echo-diff')
        if a is not None:
            env['ECHO_DIFF'] = a
        a = opts.get('mserver_set')
        if a is not None:
            env['MSERVER_SET'] = "--set " + a
        else:
            env['MSERVER_SET'] = ""
        a = opts.get('no_clean')
        if a is not None:
            env['NOCLEAN'] = a
        a = opts.get('multifarm')
        if a is not None:
            env['MULTIFARM'] = 'True'
        jenkins = opts.get('jenkins', False)
        addreqs = opts.get('addreqs', False)
    if THISFILE == 'Mapprove.py':
        a = opts.get('ext')
        if a is None:
            par['EXTENSION'] = ['out', 'err']
        elif a in ('out', 'err'):
            par['EXTENSION'] = [a]
        else:
            ErrXit("Extension (-x) must be one of: 'out', 'err' !")
        par['FORCE'] = opts.get('force', False)
        par['NOPATCH'] = opts.get('nopatch', False)
        a = opts.get('sys')
        if a is None:
            par['SYSTEM'] = ''
        else:
            par['SYSTEM'] = a
    for v in vars:
        a = opts.get(v)
        if a is not None:
            env[v] = a

    # display par's
    STDERR.flush()
    if verbose:
        for v in par.keys():
            #os.environ[v] = par[v]
            print"%s = %s" % (v, str(par[v]))
    STDOUT.flush()
    #env['par'] = par

    # unknown at compile time, as Mtest.py is compiled with MonetDB;
    # hence, we set them at runtime.
    # X == true   =>  @X_TRUE@='',  @X_FALSE@='#'
    # X == false  =>  @X_TRUE@='#', @X_FALSE@=''
    if CheckExec('php'):
        CONDITIONALS['HAVE_PHP'] = '#'
#     else:
#         CONDITIONALS['HAVE_PHP'] = ''
    CheckClassPath()

    # tidy-up and fall-back to defaults where necessary
    if THISFILE == "Mtest.py":
        vars_ = vars + ['MAPIPORT', 'GDK_DEBUG', 'GDK_NR_THREADS', 'MONETDB_MOD_PATH']
    else: # THISFILE == "Mapprove.py"
        vars_ = vars
    for v in vars_:
        if not env.has_key(v):
            env[v] = eval(dft[v])
            #TODO:
            # make sure, that PATHs are absolute
    if THISFILE == "Mtest.py":
        if env['MAPIPORT'] == 0:
            ErrXit('Cannot find a workable MAPIPORT')
        if env['MONETDB_MOD_PATH']:
            env['setMONETDB_MOD_PATH'] = eval(dft['setMONETDB_MOD_PATH'])
        else:
            env['setMONETDB_MOD_PATH'] = ''
        if env.has_key('DBG'):
            env['setDBG'] = env['DBG']
        else:
            env['setDBG'] = ''

    #TODO:
    ## in case of inconsistencies, try to fallback to "save" settings
    #
    #if not os.path.indir(TSTSRCBASE):
    #       ErrXit("Illegal TSTSRCBASE: directory '"+a"` does not exist!")

    # ensure consistent TSTSRCBASE
    if os.path.basename(env['TSTSRCBASE']) == TSTSUFF and \
            os.path.isfile(os.path.join(env['TSTSRCBASE'], "All")):
        ErrXit('TSTSRCBASE itself must not be a test-directory, i.e., called "%s" and contain an "All" file!' % TSTSUFF)

    # make TSTxxxBASE absolute physical paths
    for p in 'TSTSRCBASE', 'TSTTRGBASE':
        if os.path.isdir(env[p]):
            rp = os.path.realpath(env[p])
            if verbose and os.path.normcase(rp) != os.path.normcase(env[p]):
                Warn("%s: Replacing logical path  %s  by absolute physical path  %s" % (p, env[p], rp))
            env[p] = rp
        else:
            ErrXit("Illegal "+p+": directory '"+env[p]+"' does not exist!")


    global TSTTRGBASE
    TSTTRGBASE = env['TSTTRGBASE']
    global TSTSRCBASE
    TSTSRCBASE = env['TSTSRCBASE']

    if THISFILE == "Mapprove.py" \
       and not os.path.exists(os.path.join(TSTTRGBASE, dftTSTPREF )) \
       and     os.path.isfile(os.path.join(TSTTRGBASE, 'times.lst')):
        env['TSTPREF'] = os.path.basename(TSTTRGBASE)
        TSTTRGBASE = env['TSTTRGBASE'] = os.path.dirname(TSTTRGBASE)
    else:
        env['TSTPREF'] = dftTSTPREF
    global TSTPREF
    TSTPREF = env['TSTPREF']

    # check whether we have a Mercurial clone
    BACK = os.getcwd()
    try:
        os.chdir(TSTSRCBASE)
        proc = process.Popen(['hg', 'root'], stdout = process.PIPE)
        out, err = proc.communicate()
        if proc.returncode == 0:
            CONDITIONALS['MERCURIAL'] = '#' # True
        proc = None
    except:
        pass
    os.chdir(BACK)

    # read '.Mapprove.rc'
    if THISFILE == 'Mapprove.py':
        f = os.path.join(TSTTRGBASE, TSTPREF, '.Mapprove.rc')
        v = ReadMapproveRc(f)
        SYST = v['SYST']
        RELEASE = v['RELEASE']
        SYSTVER = SYST+RELEASE
        DIST = v['DIST']
        VERSION = v['VERSION']
        DISTVER = DIST+VERSION
        os.environ['SYST'] = SYST
        os.environ['SYSTVER'] = SYSTVER
        os.environ['RELEASE'] = RELEASE
        os.environ['DIST'] = DIST
        os.environ['DISTVER'] = DISTVER
        os.environ['VERSION'] = VERSION
        w = {}
        for i in 'SYST', 'RELEASE', 'DIST', 'VERSION', 'BITS', 'OIDS', 'INT128', 'SINGLE', 'STATIC':
            w[i] = re.escape(v[i])
        for i in 'BITS', 'OIDS', 'INT128', 'SINGLE', 'STATIC':
            j = 'TST_'+i
            env[j] = v[i]
            os.environ[j] = v[i]
            if v[i]:
                v[i] = '(.%s)?' % v[i]
                w[i] = '(\.%s)?' % w[i]
        sv = '^(.(%s(%s)?|%s(%s)?))?%s%s%s%s%s$' % (v['SYST'], v['RELEASE'], v['DIST'], v['VERSION'], v['BITS'], v['OIDS'], v['INT128'], v['SINGLE'], v['STATIC'])
        sw = '^(\.(%s(%s)?|%s(%s)?))?%s%s%s%s%s$' % (w['SYST'], w['RELEASE'], w['DIST'], w['VERSION'], w['BITS'], w['OIDS'], w['INT128'], w['SINGLE'], w['STATIC'])
        r = re.compile(sw)
        if not r.match(par['SYSTEM']):
            ErrXit("System (-S) must match '"+sv+"' !")

    # some relative path's for relocatable HTML output
    RELSRCBASE = os.path.relpath(TSTSRCBASE, TSTTRGBASE)
    env['RELSRCBASE'] = RELSRCBASE

    #STDERR.flush()
    #for v in 'RELSRCBASE':
    #       print v+" = "+str(env[v])
    #STDOUT.flush()

    # inject vars that tell various languages where to find their libs
    env['PERL5LIB'] = _configure(os.path.join('/usr/local', 'lib64/perl5/vendor_perl'))
# set dynamically for python test lib
#    env['PYTHONPATH'] = _configure(os.path.join('/usr/local', 'lib/python2.7/site-packages'))
    env['PHP_INCPATH'] = _configure(os.path.join('${prefix}/share', 'php'))
    env['BINDIR'] = _configure('${exec_prefix}/bin')
    vars_ = vars_ + ['PERL5LIB', 'PHP_INCPATH', 'BINDIR']

    # export and display env
    STDERR.flush()
    if THISFILE == "Mtest.py":
        vars_ = vars_ + ['GDK_DBFARM']
        vars_ = vars_ + ['setMONETDB_MOD_PATH']
    else: # THISFILE == "Mapprove.py"
        vars_ = vars_
    for v in vars_:
        if env.has_key(v):
            os.environ[v] = env[v]
            if verbose:
                print "%s = %s" % (v, env[v])
    if verbose:
        print "%s = %s" % ('PATH', os.environ['PATH'])
        if os.environ.has_key('PYTHONPATH'):
            print "%s = %s" % ('PYTHONPATH', os.environ['PYTHONPATH'])
        if os.environ.has_key('CLASSPATH'):
            print "%s = %s" % ('CLASSPATH', os.environ['CLASSPATH'])
    STDOUT.flush()

    # add QUIET par to env
    env['QUIET'] = quiet

    ## set/extend PATH & LD_LIBRARY_PATH
    #bp = ""        #_configure(os.path.join('/usr/local',"bin"))
    #if THISFILE == "Mtest.py":
    #       lp = env['MONETDB_MOD_PATH']
    #else: # THISFILE == "Mapprove.py"
    #       lp = ""
    #if os.name == "nt"  and  lp:
    #       if bp:
    #               bp = bp+os.pathsep+lp
    #       else:
    #               bp = lp
    #if os.environ.has_key('PATH'):
    #       if bp:
    #               bp = bp+os.pathsep+os.environ['PATH']
    #       else:
    #               bp = os.environ['PATH']
    #os.environ['PATH'] = bp
    #print "PATH = "+bp
    #if os.name == "posix":
    #       if os.environ.has_key('LD_LIBRARY_PATH'):
    #               if lp:
    #                       lp = lp+os.pathsep+os.environ['LD_LIBRARY_PATH']
    #               else:
    #                       lp = os.environ['LD_LIBRARY_PATH']
    #       os.environ['LD_LIBRARY_PATH'] = lp
    #       print "LD_LIBRARY_PATH = "+lp

    if not startswithpath(os.getcwd() + os.sep, TSTSRCBASE + os.sep):
        Warn("Current directory %s is no descendant of TSTSRCBASE=%s;" % (os.getcwd(), TSTSRCBASE))
        Warn("changing to TSTSRCBASE=%s, now." % TSTSRCBASE)
        os.chdir(TSTSRCBASE)

    global REV
    global URLPREFIX
    REV = opts.get('revision')
    if REV is None:             # no --revision option: try to find out
        try:
            proc = process.Popen(['hg', 'id', '-i'], stdout = process.PIPE)
            out, err = proc.communicate()
            if proc.returncode == 0:
                REV = out.strip()
            proc = None
        except:
            pass
    # fix up URLPREFIX
    if REV:
        URLPREFIX += '%s/' % REV.split()[0].rstrip('+')
        os.environ['REVISION'] = REV
    else:
        # if no revision known, can't refer to repository
        URLPREFIX = None

    global SOCK, HOST
    try:                        # try/finally to clean up sockdir
        # check for executables, set their standard options and export them
        if THISFILE == "Mtest.py":
            if SOCK:
                # we cannot put the UNIX socket in the mtest root, because that
                # makes the UNIX socket too long on most platforms, so use
                # /var/tmp/mtest and try not to forget to clean that up
                sockdir = "/var/tmp/mtest-%d" % os.getpid()
                try:
                    os.mkdir(sockdir);
                    SOCK = "--set mapi_usock=%s/.s.monetdb.%s" % \
                            (sockdir, env['MAPIPORT'])
                    HOST = sockdir
                    os.environ['MAPIHOST'] = HOST
                except:
                    SOCK = ""
            else:
                SOCK = ""

            exe = {}
            exe['Mtimeout']      = CheckExec('Mtimeout')     , 'Mtimeout -timeout %d ' % par['TIMEOUT']
            exe['Mserver']       = CheckExec('mserver5')     , '%s mserver5 %s --debug=%s --set gdk_nr_threads=%s %s --set mapi_open=true --set mapi_port=%s %s --set monet_prompt= %s --set mal_listing=2 %s' % \
                                                                   (env['setDBG'], config, env['GDK_DEBUG'], env['GDK_NR_THREADS'], env['setMONETDB_MOD_PATH'], env['MAPIPORT'], SOCK, not nomito and '--forcemito' or '', env['MSERVER_SET'])
            exe['Mdiff']         = CheckExec('Mdiff')        , 'Mdiff'
            exe['python']        = CheckExec(sys.executable) , sys.executable
            exe['MAL_Client']    = CheckExec(env['MALCLIENT'].split(None, 1)[0])  , '%s -i -e --host=%s --port=%s' % (env['MALCLIENT'], HOST, env['MAPIPORT'])
            exe['SQL_Client']    = CheckExec(env['SQLCLIENT'].split(None, 1)[0])   , '%s -i -e --host=%s --port=%s' % (env['SQLCLIENT'], HOST, env['MAPIPORT'])
            exe['SQL_Dump']      = CheckExec(env['SQLDUMP'].split(None, 1)[0])     , '%s --host=%s --port=%s' % (env['SQLDUMP'], HOST, env['MAPIPORT'])
            exe['R_Client']   = CheckExec(env['RCLIENT'].split(None, 1)[0])   , '%s --args %s' % (env['RCLIENT'], env['MAPIPORT'])
            if par['TIMEOUT'] == 0 or not exe['Mtimeout'][0]:
                exe['Mtimeout'] = '', ''
            env['exe'] = exe
            SetExecEnv(exe,verbose)

            #TODO:
            #exe['JAVA']       = 'java'
            #exe['JAVAC']      = 'javac'

        # parse commandline arguments
        testdirs = []
        testlist = []
        dirlist = []
        if   len(args) == 1:
            if   os.path.isdir(args[0]):
                d = os.path.realpath(args[0])
                if startswithpath(d + os.sep, TSTSRCBASE + os.sep):
                    dirlist.append(d)
                #TODO:
                #else:
                    # WARNING/ERROR
            elif args[0].find(os.sep) != -1:
                ErrXit("'%s` is neither a valid directory in %s nor a valid test-name!" % (args[0], os.getcwd()))
            elif args[0] != "All":
                #TODO:
                # check, whether args[0] in All
                testlist.append(args[0])
        elif len(args) > 1:
            i = 0
            while i < len(args)  and  os.path.isdir(args[i]):
                d = os.path.realpath(args[i])
                if startswithpath(d + os.sep, TSTSRCBASE + os.sep):
                    dirlist.append(os.path.realpath(args[i]))
                #TODO:
                #else:
                    # WARNING/ERROR
                i = i + 1
            if len(dirlist) == 1  and  i < len(args)  and  args[i] != "All":
                while i < len(args):
                    if args[i].find(os.sep) == -1:
                        #TODO:
                        # check, whether args[i] in All
                        testlist.append(args[i])
                    #TODO
                    #else:
                        # ERROR/WARNING
                    i = i + 1
            else:
                if i < len(args)  and  args[i] == "All":
                    i = i + 1
                #TODO:
                #if i < len(args):
                    #if len(dirlist) > 1:
                        # Warn: dirlist => ignore testlist, assume All
                    #else:
                        # Warn: All => ignore testlist
        else:
            # len(args) == 0: no explicit tests specified so do all
            recursive = True

        if not dirlist:
            dirlist.append(os.getcwd())
        if recursive:
            #TODO
            #if testlist:
                # WARNING
            testlist = []
            for d in dirlist:
                test_dirs = find_test_dirs(d)
                test_dirs.sort()
                for t in test_dirs:
                    if t not in testdirs:
                        testdirs.append(t)
        else:
            for d in dirlist:
                if   os.path.basename(d) == TSTSUFF  and  os.path.isfile(os.path.join(d,"All")):
                    testdirs.append(os.path.dirname(os.path.realpath(d)))
                elif os.path.isdir(os.path.join(d,TSTSUFF))  and  os.path.isfile(os.path.join(d,TSTSUFF,"All")):
                    testdirs.append(os.path.realpath(d))
                else:
                    Warn("No tests found in '%s`; skipping directory!" % d)

        if len(testdirs) > 1  and  testlist:
            testlist = []
            #TODO
            # WARNING
        if not testdirs:
            Warn("No tests found in %s!" % ', '.join(dirlist))
            sys.exit(1)

        if len(testdirs) == 1 and len(testlist) > 0 and addreqs:
            added = True
            while added:
                added = False
                i = 0
                while i < len(testlist):
                    if os.path.exists(os.path.join(testdirs[0], 'Tests', testlist[i] + '.reqtests')):
                        for t in open(os.path.join(testdirs[0], 'Tests', testlist[i] + '.reqtests')):
                            t = t[:-1] # remove newline
                            if t not in testlist[:i]:
                                testlist.insert(i, t)
                                i = i + 1
                                added = True
                            #elif t in testlist:
                                # WARNING: tests in wrong order
                    i = i + 1

        BusyPorts = []

        if THISFILE == "Mtest.py":
            if not env.get('NOCLEAN') and os.path.exists(os.path.join(TSTTRGBASE, TSTPREF)):
                try:
                    shutil.rmtree(os.path.join(TSTTRGBASE, TSTPREF))
                except:
                    ErrXit("Failed to remove %s" % os.path.join(TSTTRGBASE, TSTPREF))
            if not os.path.exists(env['GDK_DBFARM']):
                #TODO: set mode to umask
                os.makedirs(env['GDK_DBFARM'])

            if not env.get('NOCLEAN') and os.path.exists(os.path.join(env['GDK_DBFARM'], TSTPREF)):
                try:
                    shutil.rmtree(os.path.join(env['GDK_DBFARM'], TSTPREF))
                except:
                    ErrXit("Failed to remove %s" % os.path.join(env['GDK_DBFARM'], TSTPREF))
            try:
                os.makedirs(os.path.join(env['GDK_DBFARM'], TSTPREF))
            except os.error:
                if not env.get('NOCLEAN'):
                    ErrXit("Failed to create %s" % os.path.join(env['GDK_DBFARM'], TSTPREF))

            try:
                os.makedirs(os.path.join(TSTTRGBASE, TSTPREF))
            except os.error:
                if not env.get('NOCLEAN'):
                    ErrXit("Failed to create %s" % os.path.join(TSTTRGBASE, TSTPREF))

            # write .monetdb file for mclient to do authentication with
            dotmonetdbfile = os.path.join(TSTTRGBASE, ".monetdb")
            dotmonetdb = open(dotmonetdbfile, 'w')
            dotmonetdb.write('user=monetdb\n')
            dotmonetdb.write('password=monetdb\n')
            dotmonetdb.close()
            # and make mclient find it
            os.environ['DOTMONETDBFILE'] = dotmonetdbfile

            env['TST_MODS'] = []
            env['TST_BITS'] = ""
            env['TST_OIDS'] = ""
            env['TST_INT128'] = ""
            env['TST_SINGLE'] = ""
            env['TST_THREADS'] = ""
            env['TST_STATIC'] = ""
            cmd = splitcommand(env['exe']['Mserver'][1])
            cmd.append('--dbpath=%s' % os.path.join(env['GDK_DBFARM'], TSTPREF))
            if env.get('MULTIFARM'):
                cmd.append('--dbextra=%s' % os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'))
                shutil.rmtree(os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'),
                              ignore_errors = True)
                os.makedirs(os.path.join(env['GDK_DBFARM'], TSTPREF + '_transient'))
            if Check(cmd, 'clients.quit();\n'):
                sys.exit(1)
            if GetBitsAndOIDsAndModsAndStaticAndThreads(env):
                sys.exit(1)
            STDERR.flush()
            if verbose:
                print "Bits: ", env['TST_BITS']
                print "OIDs: ", env['TST_OIDS']
                if env['TST_INT128']:
                    print "Integers: 128bit"
                print "Modules: ", env['TST_MODS']
            STDOUT.flush()

            port = int(env['MAPIPORT'])
            busy, host, Serrno, Serrstr, S = CheckPort(port)
            S[0].close()
            S[1].close()
            if busy:
                Warn("Skipping MAPI tests as MAPIPORT=%s is not available on %s (Error #%d: '%s')!" % (env['MAPIPORT'],host,Serrno,Serrstr))
                BusyPorts.append('MAPI')

            # create '.Mapprove.rc'
            env['SYST'] = os.environ['SYST']
            env['RELEASE'] = os.environ['RELEASE']
            env['DIST'] = os.environ['DIST']
            env['VERSION'] = os.environ['VERSION']
            n = os.path.join(TSTTRGBASE, TSTPREF, '.Mapprove.rc')
            f = open(n, 'w')
            for v in 'SYST', 'RELEASE', 'DIST', 'VERSION', 'TST_BITS', 'TST_OIDS', 'TST_INT128', 'TST_SINGLE', 'TST_STATIC':
                w = v.replace('TST_','')
                f.write('%s = "%s"\n' % (w, env[v]))
            f.close()

        STDERR.flush()
        t_ = 0
        body_good = []
        body_bad = []
        if len(testdirs) == 1:
            if testlist:
                tsts = "tests "+str(testlist)
            else:
                tsts = "all tests"
            if verbose:
                print "\nRunning %s in directory %s.\n" % (tsts , testdirs[0])
            t_, elem, diff = PerformDir(env, testdirs[0], testlist, BusyPorts)
            if elem is not None:
                if diff <= F_OK:
                    body_good.append(elem)
                else:
                    body_bad.append(elem)
        else:
            if verbose:
                print "\nRunning all tests in directories %s.\n" % str(testdirs)
            for d in testdirs:
                t, elem, diff = PerformDir(env, d, [], BusyPorts)
                t_ = t_ + t
                if elem is not None:
                    if diff <= F_OK:
                        body_good.append(elem)
                    else:
                        body_bad.append(elem)
        body = body_bad + body_good

        if THISFILE == "Mtest.py":
            if testweb:
                # some more cleanup
                # note that we create the file so that os.removedirs in PerformDir
                # doesn't remove os.path.join(TSTTRGBASE, TSTPREF)
                remove(os.path.join(TSTTRGBASE, TSTPREF, '.Mapprove.rc'))
            fn = os.path.join(TSTTRGBASE, TSTPREF, "times.")
            fl = open(fn+"lst","w")
            Failure = [[] for i in range(len(FAILURES))]
            for TSTDIR, TST, tt, ms, FtOut, FtErr, reason in TIMES:
                fl.write('%s:\t%s\t%s\t%s\t%s\n' % (url(os.path.join(TSTDIR, TST)),
                                                    tt,
                                                    FAILURES[FtOut][0],
                                                    FAILURES[FtErr][0],
                                                    reason or ''))
                if TST != '':
                    Failure[max(FtOut,FtErr)].append(os.path.join(TSTDIR,TST))
            fl.write(":\t%7.3f\t\n" % t_)
            fl.close()

            fl = open(fn+"sql","w")
            host = socket.gethostname()
            product = os.path.split(TSTSRCBASE)[-1]

            compiler = ''
            # TODO:
            # use gcc  -g  -Werror -Wall -Wextra -W -Werror-implicit-function-declaration -Wpointer-arith -Wdeclaration-after-statement -Wundef -Wformat=2 -Wno-format-nonliteral -Winit-self -Winvalid-pch -Wmissing-declarations -Wmissing-format-attribute -Wmissing-prototypes -Wold-style-definition -Wpacked -Wunknown-pragmas -Wvariadic-macros -fstack-protector-all -Wstack-protector -Wpacked-bitfield-compat -Wsync-nand -Wjump-misses-init -Wmissing-include-dirs -Wlogical-op -Wunreachable-code for this
            # (and then also allow compiler-specific output)
            #if os.name == 'nt':
            #    compiler = 'Mic' # Microsoft Visual Studio C
            #    #compiler = 'Int' # Intel icc
            #else:
            #    f = os.path.join(env['TSTBLDBASE'],'Makefile')
            #    if os.path.isfile(f):
            #        r = re.compile("^CC = (.*)$", re.MULTILINE)
            #        for l in open(f):
            #            mt = r.match(l)
            #            if mt:
            #                compiler = mt.group(1)

            # start of times.sql output preparation
            try:
                from mx import DateTime
                now = "timestamp '" + str(DateTime.now()) + "'"
            except ImportError:
                now = 'now()'

            if env['TST_INT128'] != '':
                isInt128 = 'true'
            else:
                isInt128 = 'false'

            if env['TST_SINGLE'] != '':
                isSingle = 'true'
            else:
                isSingle = 'false'

            if env['TST_STATIC'] != '':
                isStatic = 'true'
            else:
                isStatic = 'false'

            # ok, we're not prepared for the 128 bits world yet
            bits = env['TST_BITS'][:2]
            oids = env['TST_OIDS'][-2:]

            # we write in SQL the same codes as testweb uses in the HTML
            # pages, for readability

            # we are not interested in the compiler, nor its path, nor its
            # options.  We do store the options separately, though
            hasSpace = compiler.find(' ')
            if hasSpace != -1:
                ccname = os.path.split(compiler[:hasSpace])[-1]
                ccopts = compiler[hasSpace + 1:]
            else:
                ccname = os.path.split(compiler)[-1]
                ccopts = ''

            for TSTDIR, TST, tt, ms, FtOut, FtErr, reason in TIMES:
                if FtOut == F_SKIP and FtErr == F_SKIP:
                    tms = 'NULL'
                else:
                    tms = '%d' % ms

                if TST != '':
                    # target is a platform and compilation options etc
                    fl.write("""
INSERT INTO mtest (\"date\", \"machine\", \"os\", \"release\",
    \"compiler\", \"compiler_opts\", \"bits\", \"oid\", \"int128\", \"single\", \"static\",
    \"product\", \"dir\", \"test\",
    \"time\", \"stdout\", \"stderr\")
VALUES (%s, '%s', '%s', '%s',
    '%s', '%s', %s, %s, %s, %s, %s,
    '%s', '%s', '%s',
    %s, '%s', '%s');
""" % (now, host, env['SYST'], env['RELEASE'],
           ccname, ccopts, bits, oids, isInt128, isSingle, isStatic,
           product, TSTDIR, TST,
           tms, FAILURES[FtOut][1], FAILURES[FtErr][1]))
            fl.close()

        if THISFILE == "Mtest.py":
            env['TSTDIR'] = ""
            env['TSTTRGDIR'] = os.path.join(TSTTRGBASE, TSTPREF)
            if not testweb:
                CreateHtmlIndex(env, *body)

            Failed = 0
            for f in Failure[1:-1]:
                Failed += len(f)
            num_tests = 0
            for f in Failure:
                num_tests += len(f)
            how = ""
            what = ""
            for x, y, z in [(F_SKIP, "could not be executed", ""),
                            (F_WARN, "produced slightly different output", "slightly"),
                            (F_SOCK, "did not properly release socket(s)", "SIGNIFICANTLY"),
                            (F_TIME, "ran into timeout", "SIGNIFICANTLY"),
                            (F_ABRT, "caused an abort (assertion failure)", "SIGNIFICANTLY"),
                            (F_SEGV, "resulted in a crash", "SIGNIFICANTLY"),
                            (F_RECU, "ran into too deep recursion", "SIGNIFICANTLY"),
                            (F_ERROR, "produced SIGNIFICANTLY different output", "SIGNIFICANTLY")]:
                if Failure[x]:
                    how = z
                    what += "  %3d out of %3d tests %s\n" % (len(Failure[x]),num_tests, y)
                    # only report failed tests if there aren't too many
                    if Failed < 30 and \
                           (x != F_SKIP or len(Failure[F_SKIP]) + Failed < 30):
                        for f in Failure[x]:
                            what += "        %s\n" % f
            STDERR.flush()
            if Failed:
                print """\

 !ERROR:  Testing FAILED %s (%d out of %d tests failed)

%s
""" % (how, Failed, num_tests, what)
                if not testweb:
                    print """\
 First, check the testing results in  %s  !

 Then, fix the problems by:
  - fixing sources and test scripts
  - fixing stable output by hand
  - approving test output by Mapprove.py (cf. Mapprove.py -?)

 After that, re-run Mtest.
""" % os.path.join(TSTTRGBASE, TSTPREF, "index.html")
                if jenkins:
                    sys.exit(0)
                sys.exit(1)
            else:
                if quiet:
                    pass
                elif verbose:
                    print """\

 No differences encountered during testing.

 If necessary, you can checkin your modifications, now.
"""
                else:
                    print "No differences encountered during testing."
                sys.exit(0)

        if THISFILE == "Mapprove.py":
            if not quiet:
                print """\

 First, run 'hg diff` to check what you have changed.

 Then, re-run Mtest.py.
"""
            if t_:
                if par['FORCE']:
                    print """\
 In case (some of) the approved error messages are not correct/expected,
 re-run Mapprove.py without -f to skip their approval.
"""
                else:
                    print """\
 In case (some of) the skipped error messages are correct/expected,
 re-run Mapprove.py with -f to force their approval.
"""
    finally:
        # cleanup the place where we put our UNIX sockets
        if THISFILE == "Mtest.py" and SOCK:
            try:
                shutil.rmtree(sockdir);
            except:
                pass

### main(argv) #

if __name__ == "__main__":
    if '--trace' in sys.argv:
        sys.argv.remove('--trace')
        try:
            import trace
        except ImportError:
            from MonetDBtesting import trace
        t = trace.Trace(trace=1, count=0, ignoremods=('ntpath','monet_options','Mfilter','re', 'sre_parse', 'sre_compile'))
        t.runfunc(main, sys.argv)
    elif '--debug' in sys.argv:
        sys.argv.remove('--debug')
        import pdb
        pdb.run('main(sys.argv)')
    else:
        main(sys.argv)

#       END
#############################################################################
# vim: set ts=4 sw=4 expandtab:
