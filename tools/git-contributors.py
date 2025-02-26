#!/usr/bin/env python3

# List all contributors to a series of git commits.
# Copyright(C) 2025 Oracle, All Rights Reserved.
# Licensed under GPL 2.0 or later

import re
import subprocess
import io
import sys
import argparse
import email.utils

DEBUG = False

def backtick(args):
    '''Generator function that yields lines of a program's stdout.'''
    if DEBUG:
        print(' '.join(args))
    p = subprocess.Popen(args, stdout = subprocess.PIPE)
    for line in io.TextIOWrapper(p.stdout, encoding="utf-8"):
        yield line

class find_developers(object):
    def __init__(self):
        tags = '%s|%s|%s|%s|%s|%s|%s|%s' % (
            'signed-off-by',
            'acked-by',
            'cc',
            'reviewed-by',
            'reported-by',
            'tested-by',
            'suggested-by',
            'reported-and-tested-by')
        # some tag, a colon, a space, and everything after that
        regex1 = r'^(%s):\s+(.+)$' % tags

        self.r1 = re.compile(regex1, re.I)

        # regex to guess if this is a list of multiple addresses.
        # Not sure why the initial "^.*" is needed here.
        self.r2 = re.compile(r'^.*,[^,]*@[^@]*,[^,]*@', re.I)

        # regex to match on anything inside a pair of angle brackets
        self.r3 = re.compile(r'^.*<(.+)>', re.I)

    def _handle_addr(self, addr):
        # The next split removes everything after an octothorpe (hash
        # mark), because someone could have provided an improperly
        # formatted email address:
        #
        # Cc: stable@vger.kernel.org # v6.19+
        #
        # This, according to my reading of RFC5322, is allowed because
        # octothorpes can be part of atom text.  However, it is
        # interepreted as if there weren't any whitespace
        # ("stable@vger.kernel.org#v6.19+").  The grammar allows for
        # this form, even though this is not a correct Internet domain
        # name.
        #
        # Worse, if you follow the format specified in the kernel's
        # SubmittingPatches file:
        #
        # Cc: <stable@vger.kernel.org> # v6.9
        #
        # emailutils will not know how to parse this, and returns empty
        # strings.  I think this is because the angle-addr
        # specification allows only whitespace between the closing
        # angle bracket and the CRLF.
        #
        # Hack around both problems by ignoring everything after an
        # octothorpe, no matter where it occurs in the string.  If
        # someone has one in their name or the email address, too bad.
        a = addr.split('#')[0]

        # emailutils can extract email addresses from headers that
        # roughly follow the destination address field format:
        #
        # Reviewed-by: Bogus J. Simpson <bogus@simpson.com>
        # Reviewed-by: "Bogus J. Simpson" <bogus@simpson.com>
        # Reviewed-by: bogus@simpson.com
        #
        # Use it to extract the email address, because we don't care
        # about the display name.
        (name, addr) = email.utils.parseaddr(a)
        if DEBUG:
            print(f'A:{a}:NAME:{name}:ADDR:{addr}:')
        if len(addr) > 0:
            return addr

        # If emailutils fails to find anything, let's see if there's
        # a sequence of characters within angle brackets and hope that
        # is an email address.  This works around things like:
        #
        # Reported-by: Xu, Wen <wen.xu@gatech.edu>
        #
        # Which should have had the name in quotations because there's
        # a comma.
        m = self.r3.match(a)
        if m:
            addr = m.expand(r'\g<1>')
            if DEBUG:
                print(f"M3:{addr}:M:{m}:")
            return addr

        # No idea, just spit the whole thing out and hope for the best.
        return a

    def run(self, lines):
        addr_list = []

        for line in lines:
            l = line.strip()

            # First, does this line match any of the headers we
            # know about?
            m = self.r1.match(l)
            if not m:
                continue
            rightside = m.expand(r'\g<2>')

            n = self.r2.match(rightside)
            if n:
                # Break the line into an array of addresses,
                # delimited by commas, then handle each
                # address.
                addrs = rightside.split(',')
                if DEBUG:
                    print(f"0LINE:{rightside}:ADDRS:{addrs}:M:{n}")
                for addr in addrs:
                    a = self._handle_addr(addr)
                    addr_list.append(a)
            else:
                # Otherwise treat the line as a single email
                # address.
                if DEBUG:
                    print(f"1LINE:{rightside}:M:{n}")
                a = self._handle_addr(rightside)
                addr_list.append(a)

        return sorted(set(addr_list))

def main():
    global DEBUG

    parser = argparse.ArgumentParser(description = "List email addresses of contributors to a series of git commits.")
    parser.add_argument("revspec", help = "git revisions to process.")
    parser.add_argument("--separator", type = str, default = '\n', \
            help = "Separate each email address with this string.")
    parser.add_argument('--debug', action = 'store_true', default = False, \
            help = argparse.SUPPRESS)
    args = parser.parse_args()

    if args.debug:
        DEBUG = True

    fd = find_developers()
    if args.revspec:
        # read git commits from repo
        contributors = fd.run(backtick(['git', 'log', '--pretty=medium',
                  args.revspec]))

    print(args.separator.join(sorted(contributors)))
    return 0

if __name__ == '__main__':
    sys.exit(main())

