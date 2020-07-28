#!/usr/bin/python3

'''
 Copyright (C) 2009 Intel Corporation

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) version 3.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 02110-1301  USA
'''
import sys,os,glob,datetime
import re
import fnmatch
import cgi
import subprocess

""" 
resultcheck.py: tranverse the test result directory, generate an XML
based test report.
"""

# sort more accurately on sub-second modification times
os.stat_float_times(True)

space="  "
def check (resultdir, serverlist,resulturi, srcdir, shellprefix, backenddir):
    '''Entrypoint, resutldir is the test result directory to be generated,
    resulturi is the http uri, it will only process corresponding server's
    test results list in severlist'''
    if serverlist:
        servers = serverlist.split(",")
    else:
        servers = []
    result = open("%s/nightly.xml" % resultdir,"w",encoding='utf-8')
    result.write('''<?xml version="1.0" encoding="utf-8" ?>\n''')
    result.write('''<nightly-test>\n''')
    indents=[space]
    if(os.path.isfile(resultdir+"/output.txt")==False):
        print("main test output file not exist!")
    else:
        indents,cont = step1(resultdir,result,indents,resultdir,resulturi, shellprefix, srcdir)
        if (cont):
            step2(resultdir,result,servers,indents,srcdir,shellprefix,backenddir)
        else:
            # compare.xsl fails if there is no <client-test> element:
            # add an empty one
            result.write('''<client-test/>\n''')
    result.write('''</nightly-test>\n''')
    result.close()

patchsummary = re.compile('^Subject: (?:\[PATCH.*?\] )?(.*)\n')
patchauthor = re.compile('^From: (.*?) <.*>\n')
def extractPatchSummary(patchfile):
    author = ""
    for line in open(patchfile, encoding='utf-8'):
        m = patchauthor.match(line)
        if m:
            author = m.group(1) + " - "
        else:
            m = patchsummary.match(line)
            if m:
                return author + m.group(1)
    return os.path.basename(patchfile)

def step1(resultdir, result, indents, dir, resulturi, shellprefix, srcdir):
    '''Step1 of the result checking, collect system information and 
    check the preparation steps (fetch, compile)'''

    # Always keep checking, even if any of the preparation steps failed.
    cont = True

    input = os.path.join(resultdir, "output.txt")
    indent =indents[-1]+space
    indents.append(indent)

    # include information prepared by GitCopy in runtests.py
    result.write(indent+'<source-info>\n')
    files = os.listdir(resultdir)
    files.sort()
    for source in files:
        m = re.match('(.*)-source.txt', source)
        if m:
            name = m.group(1)
            result.write('   <source name="%s"><description><![CDATA[%s]]></description>\n' %
                         (name, open(os.path.join(resultdir, source), encoding='utf-8').read()))
            result.write('       <patches>\n')
            for patch in files:
                if fnmatch.fnmatch(patch, name + '-*.patch'):
                    result.write('          <patch><path>%s</path><summary><![CDATA[%s]]></summary></patch>\n' %
                                 ( patch, extractPatchSummary(os.path.join(resultdir, patch)) ) )
            result.write('       </patches>\n')
            result.write('   </source>\n')
    result.write(indent+'</source-info>\n')

    result.write(indent+'''<platform-info>\n''')
    indent =indents[-1]+space
    indents.append(indent)
    result.write(indent+'''<cpuinfo>\n''')
    fout=subprocess.check_output('cat /proc/cpuinfo|grep "model name" |uniq', shell=True).decode('utf-8')
    result.write(indent+fout)
    result.write(indent+'''</cpuinfo>\n''')
    result.write(indent+'''<memoryinfo>\n''')
    fout=subprocess.check_output('cat /proc/meminfo|grep "Mem"', shell=True).decode('utf-8')
    for s in fout.split('\n'):
        result.write(indent+s)
    result.write(indent+'''</memoryinfo>\n''')
    result.write(indent+'''<osinfo>\n''')
    fout=subprocess.check_output('uname -osr'.split()).decode('utf-8')
    result.write(indent+fout)
    result.write(indent+'''</osinfo>\n''')
    if 'schroot' in shellprefix:
        result.write(indent+'''<chrootinfo>\n''')
        # Don't make assumption about the schroot invocation. Instead
        # extract the schroot name from the environment of the shell.
        name=subprocess.check_output(shellprefix + "sh -c 'echo $SCHROOT_CHROOT_NAME'",
                                     shell=True).decode('utf-8')
        info = re.sub(r'schroot .*', 'schroot -i -c ' + name, shellprefix)
        fout=subprocess.check_output(info, shell=True).decode('utf-8')
        s = []
        for line in fout.split('\n'):
            m = re.match(r'^\s+(Name|Description)\s+(.*)', line)
            if m:
                s.append(indent + m.group(1) + ': ' + m.group(2))
        result.write('\n'.join(s))
        result.write(indent+'''</chrootinfo>\n''')
    result.write(indent+'''<libraryinfo>\n''')
    libs = ['libsoup-2.4', 'evolution-data-server-1.2', 'glib-2.0','dbus-glib-1']
    s=''
    for lib in libs:
        try:
            fout=subprocess.check_output(shellprefix+' pkg-config --modversion '+lib +' |grep -v pkg-config',
                                         shell=True).decode('utf-8')
            s = s + lib +': '+fout +'  '
        except subprocess.CalledProcessError:
            pass
    result.write(indent+s)
    result.write(indent+'''</libraryinfo>\n''')
    indents.pop()
    indent = indents[-1]
    result.write(indent+'''</platform-info>\n''')
    result.write(indent+'''<prepare>\n''')
    indent =indent+space
    indents.append(indent)
    tags=['libsynthesis', 'syncevolution', 'activesyncd', 'compile', 'dist', 'distcheck']
    tagsp={'libsynthesis':'libsynthesis-source',
           'syncevolution':'syncevolution-source',
           'activesyncd':'activesyncd-source',
           'compile':'compile',
           'distcheck': 'distcheck',
           'dist':'dist'}
    for tag in tags:
        result.write(indent+'''<'''+tagsp[tag])
        fout=subprocess.check_output('find `dirname '+input+'` -type d -name *'+tag, shell=True).decode('utf-8')
        s = fout.rpartition('/')[2].rpartition('\n')[0]
        result.write(' path ="'+s+'">')
        '''check the result'''
        if 0 == os.system("grep -q '^"+tag+": .*: failed' "+input):
            result.write("failed")
        elif 0 == os.system ("grep -q '^"+tag+" successful' "+input):
            result.write("okay")
        elif 0 == os.system("grep -q '^"+tag+".* disabled in configuration$' "+input):
            result.write("skipped")
        else:
            # Not listed at all? Fail.
            result.write("failed")
        result.write('''</'''+tagsp[tag]+'''>\n''')
    indents.pop()
    indent = indents[-1]
    result.write(indent+'''</prepare>\n''')
    result.write(indent+'''<log-info>\n''')
    indent =indent+space
    indents.append(indent)
    result.write(indent+'''<uri>'''+resulturi+'''</uri>\n''')
    indents.pop()
    indent = indents[-1]
    result.write(indent+'''</log-info>\n''')
    indents.pop()
    indent = indents[-1]
    return (indents, cont)

def step2(resultdir, result, servers, indents, srcdir, shellprefix, backenddir):
    '''Step2 of the result checking, for each server listed in
    servers, tranverse the corresponding result folder, process
    each log file to decide the status of the testcase'''
    '''Read the runtime parameter for each server '''
    params = {}
    if servers:
        cmd='sed -n '
        for server in servers:
            cmd+= '-e /^'+server+'/p '
        print("Analyzing overall result %s" % (resultdir+'/output.txt'))
        cmd = cmd +resultdir+'/output.txt'
        fout=subprocess.check_output(cmd, shell=True).decode('utf-8')
        for line in fout.split('\n'):
            for server in servers:
                # Find first line with "foobar successful" or "foobar: <command failure>",
                # ignore "skipped".
                if (line.startswith(server + ":") or line.startswith(server + " ")) and server not in params:
                    t = line.partition(server)[2]
                    if(t.startswith(':')):
                        t=t.partition(':')[2]
                    t = t.strip()
                    if t != 'skipped: disabled in configuration':
                        print("Result for %s: %s" % (server, t))
                        params[server]=t

    indent =indents[-1]+space
    indents.append(indent)
    '''start of testcase results '''
    result.write(indent+'''<client-test>\n''')
    runservers = os.listdir(resultdir)
    #list source test servers statically, we have no idea how to differenciate
    #automatically whether the server is a source test or sync test.
    sourceServers = ['evolution',
                     'eds',
                     'kde',
                     'file',
                     'unittests',
                     'evolution-prebuilt-build',
                     'yahoo',
                     'owndrive',
                     'davical',
                     'googlecalendar',
                     'googlecontacts',
                     'googleeas',
                     'apple',
                     'egroupware-dav',
                     'oracle',
                     'exchange',
                     'pim',
                     'dbus']
    sourceServersRun = 0
    haveSource = False
    #Only process servers listed in the input parameter and in the sourceServer
    #list and have a result folder
    for server in servers:
        matched = False
        for rserver in runservers:
            for source in sourceServers:
                if (rserver.find('-')!=-1 and server == rserver.partition('-')[2] and server == source):
                    matched = True
                    break
        if(matched):
            #put the servers at the front of the servers list, so that we will
            #process test first
            servers.remove(server)
            servers.insert (0, server);
            sourceServersRun = sourceServersRun+1;
            haveSource = True

    #process source tests first 
    if (haveSource) :
        indent +=space
        indents.append(indent)
        result.write(indent+'''<source>\n''')

    haveSync = False
    for server in servers:
        matched = False
        '''Only process servers listed in the input parametr'''
        for rserver in runservers:
            if(rserver.find('-')!= -1 and rserver.partition('-')[2] == server):
                matched = True
                break;
        if(matched):
            sourceServersRun = sourceServersRun -1;
            if (sourceServersRun == -1):
                haveSync = True
                '''generate a template which lists all test cases we supply, this helps 
                generate a comparable table and track potential uncontentional skipping
                of test cases'''
                templates=[]
                oldpath = os.getcwd()
                # Get list of Client::Sync tests one source at a time (because
                # the result might depend on CLIENT_TEST_SOURCES and which source
                # is listed there first) and combine the result for the common
                # data types (because some tests are only enable for contacts, others
                # only for events).
                # The order of the tests matters, so don't use a hash and start with
                # a source which has only the common tests enabled. Additional tests
                # then get added at the end.
                clientSync = re.compile(r' +Client::Sync::(.*?)::(?:(Suspend|Resend|Retry)::)?([^:]+)')
                for source in ('file_task', 'file_event', 'file_contact', 'eds_contact', 'eds_event'):
                    cmd = shellprefix + " env LD_LIBRARY_PATH=%s/build-synthesis/src/.libs SYNCEVOLUTION_BACKEND_DIR=%s CLIENT_TEST_PEER_CAN_RESTART=1 CLIENT_TEST_RETRY=t CLIENT_TEST_RESEND=t CLIENT_TEST_SUSPEND=t CLIENT_TEST_SOURCES=%s %s/client-test -h" % (srcdir, backenddir, source, srcdir)
                    fout=subprocess.check_output(cmd, shell=True).decode('utf-8')
                    for line in fout.split('\n'):
                        m = clientSync.match(line)
                        if m:
                            if m.group(2):
                                # special sub-grouping
                                test = m.group(2) + '__' + m.group(3)
                            else:
                                test = m.group(3)
                            if test and test not in templates:
                                templates.append(test)
                indent +=space
                indents.append(indent)
                result.write(indent+'<sync>\n')
                result.write(indent+space+'<template>')
                for template in templates:
                    result.write(indent+space+'<'+template+'/>')
                result.write('</template>\n')
            indent +=space
            indents.append(indent)
            result.write(indent+'<'+server+' path="' +rserver+'" ')
            #valgrind check resutls
            if not params.get(server, None):
                # Unknown result, treat as failure.
                result.write('result="1"')
            else:
                m = re.search(r'return code (\d+)', params[server])
                if m:
                    result.write('result="%s"' % m.group(1))
            result.write('>\n')
            # sort files by creation time, to preserve run order
            logs = [(os.stat(file).st_mtime, file) for file in glob.glob(resultdir+'/'+rserver+'/*.txt')]
            logs.sort()
            logs = [entry[1] for entry in logs]
            logdic ={}
            logprefix ={}
            if server in ('dbus', 'pim'):
                # Extract tests and their results from output.txt,
                # which contains Python unit test output. Example follows.
                # Note that there can be arbitrary text between the test name
                # and "ok" resp. "FAIL/ERROR". Therefore failed tests
                # are identified not by those words but rather by the separate
                # error reports at the end of the output. Those reports
                # are split out into separate .txt files for easy viewing
                # via the .html report.
                #
                # TestDBusServer.testCapabilities - Server.Capabilities() ... ok
                # TestDBusServer.testGetConfigScheduleWorld - Server.GetConfigScheduleWorld() ... ok
                # TestDBusServer.testGetConfigsEmpty - Server.GetConfigsEmpty() ... ok
                # TestDBusServer.testGetConfigsTemplates - Server.GetConfigsTemplates() ... FAIL
                # TestDBusServer.testInvalidConfig - Server.NoSuchConfig exception ... ok
                # TestDBusServer.testVersions - Server.GetVersions() ... ok
                #
                #======================================================================
                # FAIL: TestDBusServer.testGetConfigsTemplates - Server.GetConfigsTemplates()
                # ---------------------------------------------------------------------
                #
                # More recent Python 2.7 produces:
                # FAIL: testSyncSecondSession (__main__.TestSessionAPIsReal)

                # first build list of all tests, assuming that they pass
                dbustests = {}
                test_start = re.compile(r'''^Test(?P<cl>.*)\.test(?P<func>[^ ]*).*ok(?:ay)?$''')
                # FAIL/ERROR + description of test (old Python)
                test_fail = re.compile(r'''(?P<type>FAIL|ERROR): Test(?P<cl>.*)\.test(?P<func>[^ ]*)''')
                # FAIL/ERROR + function name of test (Python 2.7)
                test_fail_27 = re.compile(r'''(?P<type>FAIL|ERROR): test(?P<func>[^ ]*) \(.*\.(?:Test(?P<cl>.*))\)''')
                name = None
                logfile = None
                htmlfile = None
                linetype = None
                for line in open(rserver + "/output.txt", encoding='utf-8'):
                    m = test_start.search(line)
                    if m:
                        is_okay = True
                    else:
                        m = test_fail.search(line) or test_fail_27.search(line)
                        is_okay = False
                    if m:
                        # create new (?!) log file
                        cl = m.group("cl")
                        func = m.group("func")
                        newname = rserver + "/" + cl + "_" + func + ".txt"
                        if newname != name:
                            name = newname
                            logfile = open(name, "w", encoding='utf-8')
                            if htmlfile:
                                htmlfile.write('</pre></body')
                                htmlfile.close()
                                htmlfile = None
                            htmlfile = open(name + ".html", "w", encoding='utf-8')
                            htmlfile.write('''<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01//EN" "http://www.w3.org/TR/html4/strict.dtd">
<html>
<head>
   <meta http-equiv="content-type" content="text/html; charset=None">
   <style type="text/css">
td.linenos { background-color: #f0f0f0; padding-right: 10px; }
span.lineno { background-color: #f0f0f0; padding: 0 5px 0 5px; }
pre { line-height: 125%; }
body .hll { background-color: #ffffcc }
body  { background: #f8f8f8; }
span.INFO { background: #c0c0c0 }
span.ERROR { background: #e0c0c0 }
span.hl { color: #c02020 }
   </style>
</head>
<body>
<pre>''')
                            if not dbustests.get(cl):
                                dbustests[cl] = {}
                            if is_okay:
                                # okay: write a single line with the full test description
                                dbustests[cl][func] = "okay"
                                logfile.write(line)
                                logfile = None
                                htmlfile.write('<span class="OKAY">%s</span></pre></body>' % cgi.escape(line))
                                htmlfile.close()
                                htmlfile = None
                            else:
                                # failed: start writing lines into separate log file
                                dbustests[cl][func] = m.group("type")
                                linetype = "ERROR"
                                htmlfile.write('<a href="#dbus-traffic">D-Bus traffic</a> <a href="#stdout">output</a>\n\n')

                    if logfile:
                        logfile.write(line)
                        if line == 'D-Bus traffic:\n':
                            linetype = "DBUS"
                            htmlfile.write('<h3 id="dbus-traffic">D-Bus traffic:</h3>')
                        elif line == 'server output:\n':
                            linetype = "OUT"
                            htmlfile.write('<h3 id="stdout">server output:</h3>')
                        else:
                            htmlfile.write('<span class="%s">%s</span>' % (linetype, cgi.escape(line)))

                if htmlfile:
                    htmlfile.write('</pre></body')
                    htmlfile.close()
                    htmlfile = None

                # now write XML
                indent +=space
                indents.append(indent)
                for testclass in dbustests:
                    result.write('%s<%s prefix="">\n' %
                                 (indent, testclass))
                    indent +=space
                    indents.append(indent)
                    for testfunc in dbustests[testclass]:
                        result.write('%s<%s>%s</%s>\n' %
                                     (indent, testfunc,
                                      dbustests[testclass][testfunc], 
                                      testfunc))
                    indents.pop()
                    indent = indents[-1]
                    result.write('%s</%s>\n' %
                                 (indent, testclass))
                indents.pop()
                indent = indents[-1]
            else:
                for log in logs:
                    logname = os.path.basename(log)
                    if logname in ['____compare.txt',
                                   'syncevo.txt', # D-Bus server output
                                   'dbus.txt', # dbus-monitor output
                                   ]:
                        continue
                    # <path>/Client_Sync_eds_contact_testItems.txt
                    # <path>/SyncEvo_CmdlineTest_testConfigure.txt
                    # <path>/N7SyncEvo11CmdlineTestE_testConfigure.txt - C++ name mangling?
                    m = re.match(r'^(Client_Source_|Client_Sync_|N7SyncEvo\d+|[^_]*_)(.*)_([^_]*)\.txt', logname)
                    if not m or logname.endswith('.server.txt'):
                        print("skipping", logname)
                        continue
                    # Client_Sync_, Client_Source_, SyncEvo_, ...
                    prefix = m.group(1)
                    # eds_contact, CmdlineTest, ...
                    format = m.group(2)
                    # testImport
                    casename = m.group(3)
                    # special case grouping of some tests: include group inside casename instead of
                    # format, example:
                    # <path>/Client_Source_apple_caldav_LinkedItemsDefault_testLinkedItemsParent
                    m = re.match(r'(.*)_(LinkedItems\w+|Suspend|Resend|Retry)', format)
                    if m:
                        format = m.group(1)
                        casename = m.group(2) + '::' + casename
                        if '.' in casename:
                            # Ignore sub logs.
                            print("skipping", logname)
                            continue
                    # Another special case: suspend/resend/retry uses an intermediate grouping
                    # which we can ignore because the name is repeated in the test case name.
                    # m = re.match(r'(.*)_(Suspend|Resend|Retry)', format)
                    # if m:
                    #     format = m.group(1)
                    #     # Case name *not* extended, in contrast to the
                    #     # LinkedItems case above.
                    #     if '.' in casename:
                    #         # Ignore sub logs.
                    #         print "skipping", logname
                    #         continue
                    print("analyzing log %s: prefix %s, subset %s, testcase %s" % (logname, prefix, format, casename))
                    if(format not in logdic):
                        logdic[format]=[]
                    logdic[format].append((casename, log))
                    logprefix[format]=prefix
            for format in list(logdic.keys()):
                indent +=space
                indents.append(indent)
                prefix = logprefix[format]
                qformat = format;
                # avoid + sign in element name (not allowed by XML);
                # code reading XML must replace _- with + and __ with _
                qformat = qformat.replace("_", "__");
                qformat = qformat.replace("+", "_-");
                result.write(indent+'<'+qformat+' prefix="'+prefix+'">\n')
                for casename, log in logdic[format]:
                    indent +=space
                    indents.append(indent)
                    # must avoid :: in XML
                    tag = casename.replace('::', '__')
                    result.write(indent+'<'+tag+'>')
                    match=format+'::'+casename
                    matchOk=match+": okay \*\*\*"
                    matchKnownFailure=match+": \*\*\* failure ignored \*\*\*"
                    if not os.system("grep -q '" + matchKnownFailure + "' "+log):
                       result.write('knownfailure')
                    elif not os.system("tail -10 %s | grep -q 'external transport failure (local, status 20043)'" % log):
                        result.write('network')
                    elif os.system("grep -q '" + matchOk + "' "+log):
                       result.write('failed')
                    else:
                        result.write('okay')
                    result.write('</'+tag+'>\n')
                    indents.pop()
                    indent = indents[-1]
                result.write(indent+'</'+qformat+'>\n')
                indents.pop()
                indent = indents[-1]
            result.write(indent+'</'+server+'>\n')
            indents.pop()
            indent = indents[-1]
            if(sourceServersRun == 0):
                #all source servers have been processed, end the source tag and
                #start the sync tags
                result.write(indent+'''</source>\n''')
                indents.pop()
                indent = indents[-1]
    if(haveSync):
        result.write(indent+'</sync>\n')
        indents.pop()
        indent=indents[-1]
    result.write(indent+'''</client-test>\n''')
    indents.pop()
    indents = indents[-1]

if(__name__ == "__main__"):
    if (len(sys.argv)!=7):
        # srcdir and basedir must be usable inside the shell started by shellprefix (typically
        # the chroot).
        print("usage: python resultchecker.py resultdir servers resulturi srcdir shellprefix backenddir")
    else:
        check(*sys.argv[1:])
