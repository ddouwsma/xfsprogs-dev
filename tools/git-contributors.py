#!/usr/bin/python3

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

    def run(self, lines):
        addr_list = []

        for line in lines:
            l = line.strip()

            # emailutils can handle abominations like:
            #
            # Reviewed-by: Bogus J. Simpson <bogus@simpson.com>
            # Reviewed-by: "Bogus J. Simpson" <bogus@simpson.com>
            # Reviewed-by: bogus@simpson.com
            # Cc: <stable@vger.kernel.org> # v6.9
            # Tested-by: Moo Cow <foo@bar.com> # powerpc
            m = self.r1.match(l)
            if not m:
                continue
            (name, addr) = email.utils.parseaddr(m.expand(r'\g<2>'))

            # This last split removes anything after a hash mark,
            # because someone could have provided an improperly
            # formatted email address:
            #
            # Cc: stable@vger.kernel.org # v6.19+
            #
            # emailutils doesn't seem to catch this, and I can't
            # fully tell from RFC2822 that this isn't allowed.  I
            # think it is because dtext doesn't forbid spaces or
            # hash marks.
            addr_list.append(addr.split('#')[0])

        return sorted(set(addr_list))

def main():
    global DEBUG

    parser = argparse.ArgumentParser(description = "List email addresses of contributors to a series of git commits.")
    parser.add_argument("revspec", nargs = '?', default = None, \
            help = "git revisions to process.")
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
    else:
        # read patch from stdin
        contributors = fd.run(sys.stdin.readlines())

    print(args.separator.join(sorted(contributors)))
    return 0

if __name__ == '__main__':
    sys.exit(main())

