#!/bin/sh
#
# Automate generation of a new release
#
# Need to first update these files:
#
# VERSION (with new version number)
# docs/CHANGES (with changelog and version/date string)
# configure.ac (with new version string)
# debian/changelog (with new release entry, only for release version)

set -e

KUP=0
COMMIT=1
LAST_HEAD=""
FOR_NEXT=0

help() {
	echo "$(basename $0) - prepare xfsprogs release tarball or for-next update"
	printf "\t[--kup|-k] upload final tarball with KUP\n"
	printf "\t[--no-commit|-n] don't create release commit\n"
	printf "\t[--last-head|-l] commit of the last release\n"
	printf "\t[--for-next|-f] generate announce email for for-next update\n"
}

update_version() {
	echo "Updating version files"
	# doc/CHANGES
	header="xfsprogs-${version} ($(date +'%d %b %Y'))"
	sed -i "1s/^/$header\n\t<TODO list user affecting changes>\n\n/" doc/CHANGES
	$EDITOR doc/CHANGES

	# ./configure.ac
	CONF_AC="AC_INIT([xfsprogs],[${version}],[linux-xfs@vger.kernel.org])"
	sed -i "s/^AC_INIT.*/$CONF_AC/" ./configure.ac

	# ./debian/changelog
	sed -i "1s/^/\n/" ./debian/changelog
	sed -i "1s/^/ -- Nathan Scott <nathans@debian.org>  `date -R`\n/" ./debian/changelog
	sed -i "1s/^/\n/" ./debian/changelog
	sed -i "1s/^/  * New upstream release\n/" ./debian/changelog
	sed -i "1s/^/\n/" ./debian/changelog
	sed -i "1s/^/xfsprogs (${version}-1) unstable; urgency=low\n/" ./debian/changelog
}

prepare_mail() {
	branch="$1"
	mail_file=$(mktemp)
	if [ -n "$LAST_HEAD" ]; then
		if [ $branch == "master" ]; then
			reason="$(git describe --abbrev=0 $branch) released"
		else
			reason="for-next updated to $(git log --oneline --format="%h" -1 $branch)"
		fi;
		cat << EOF > $mail_file
To: linux-xfs@vger.kernel.org
Cc: $(./tools/git-contributors.py $LAST_HEAD..$branch --separator ', ')
Subject: [ANNOUNCE] xfsprogs: $reason

Hi folks,

The xfsprogs $branch branch in repository at:

	git://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git

has just been updated.

Patches often get missed, so if your outstanding patches are properly reviewed
on the list and not included in this update, please let me know.

The for-next branch has also been updated to match the state of master.

The new head of the $branch branch is commit:

$(git log --oneline --format="%H" -1 $branch)

New commits:

$(git shortlog --format="[%h] %s" $LAST_HEAD..$branch)

Code Diffstat:

$(git diff --stat --summary -C -M $LAST_HEAD..$branch)
EOF
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
		--kup|-k)
			KUP=1
			;;
		--no-commit|-n)
			COMMIT=0
			;;
		--last-head|-l)
			LAST_HEAD=$2
			shift
			;;
		--for-next|-f)
			FOR_NEXT=1
			;;
		--help|-h)
			help
			exit 0
			;;
		*)
			>&2 printf "Error: Invalid argument\n"
			exit 1
			;;
		esac
	shift
done

if [ $FOR_NEXT -eq 1 ]; then
	echo "Push your for-next branch:"
	printf "\tgit push origin for-next:for-next\n"
	prepare_mail "for-next"
	if [ -n "$LAST_HEAD" ]; then
		echo "Command to send ANNOUNCE email"
		printf "\tneomutt -H $mail_file\n"
	fi
	exit 0
fi

if [ -z "$EDITOR" ]; then
	EDITOR=$(command -v vi)
fi

if [ $COMMIT -eq 1 ]; then
	if git diff --exit-code ./VERSION > /dev/null; then
		$EDITOR ./VERSION
	fi
fi

. ./VERSION

version=${PKG_MAJOR}.${PKG_MINOR}.${PKG_REVISION}
date=`date +"%-d %B %Y"`

if [ $COMMIT -eq 1 ]; then
	update_version

	git diff --color=always | less -r
	[[ "$(read -e -p 'All good? [Y/n]> '; echo $REPLY)" == [Nn]* ]] && exit 0

	echo "Commiting new version update to git"
	git commit --all --signoff --message="xfsprogs: Release v${version}

Update all the necessary files for a v${version} release."

	echo "Tagging git repository"
	git tag --annotate --sign --message="Release v${version}" v${version}
fi

echo "Cleaning up"
make realclean
rm -rf "xfsprogs-${version}.tar" \
	"xfsprogs-${version}.tar.gz" \
	"xfsprogs-${version}.tar.asc" \
	"xfsprogs-${version}.tar.sign"


echo "Making source tarball"
make dist
gunzip -k "xfsprogs-${version}.tar.gz"

echo "Sign the source tarball"
gpg \
	--detach-sign \
	--armor \
	"xfsprogs-${version}.tar"

echo "Verify signature"
gpg \
	--verify \
	"xfsprogs-${version}.tar.asc"
if [ $? -ne 0 ]; then
	echo "Can not verify signature of tarball"
	exit 1
fi

mv "xfsprogs-${version}.tar.asc" "xfsprogs-${version}.tar.sign"

if [ $KUP -eq 1 ]; then
	kup put \
		xfsprogs-${version}.tar.gz \
		xfsprogs-${version}.tar.sign \
		pub/linux/utils/fs/xfs/xfsprogs/
fi;

prepare_mail "master"

echo ""
echo "Done. Please remember to push out tags and the branch."
printf "\tgit push origin v${version} master:master master:for-next\n"
if [ -n "$LAST_HEAD" ]; then
	echo "Command to send ANNOUNCE email"
	printf "\tneomutt -H $mail_file\n"
fi
