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

. ./VERSION

version=${PKG_MAJOR}.${PKG_MINOR}.${PKG_REVISION}
date=`date +"%-d %B %Y"`

KUP=0

help() {
	echo "$(basename $0) - prepare xfsprogs release tarball or for-next update"
	printf "\t[--kup|-k] upload final tarball with KUP\n"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--kup|-k)
			KUP=1
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

echo "Cleaning up"
make realclean
rm -rf "xfsprogs-${version}.tar" \
	"xfsprogs-${version}.tar.gz" \
	"xfsprogs-${version}.tar.asc" \
	"xfsprogs-${version}.tar.sign"

echo "Updating CHANGES"
sed -e "s/${version}.*/${version} (${date})/" doc/CHANGES > doc/CHANGES.tmp && \
	mv doc/CHANGES.tmp doc/CHANGES

echo "Commiting CHANGES update to git"
git commit --all --signoff --message="xfsprogs: Release v${version}

Update all the necessary files for a v${version} release."

echo "Tagging git repository"
git tag --annotate --sign --message="Release v${version}" v${version}

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

echo "Done. Please remember to push out tags using \"git push origin v${version}\""
