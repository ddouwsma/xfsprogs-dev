// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_arch.h"
#include "list.h"
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <strings.h>
#include <unicode/uclean.h>
#include <unicode/ustring.h>
#include <unicode/unorm2.h>
#include <unicode/uspoof.h>
#include "libfrog/paths.h"
#include "xfs_scrub.h"
#include "common.h"
#include "descr.h"
#include "unicrash.h"

/*
 * Detect Unicode confusable names in directories and attributes.
 *
 * Record all the name->ino mappings in a directory/xattr, with a twist!  The
 * twist is to record the Unicode skeleton and normalized version of every
 * name we see so that we can check for a name space (directory, extended
 * attribute set) containing names containing malicious characters or that
 * could be confused for one another.  These entries are at best a sign of
 * Unicode mishandling, or some sort of weird name substitution attack if the
 * entries do not point to the same inode.  Warn if we see multiple dirents
 * that do not all point to the same inode.
 *
 * For extended attributes we perform the same collision checks on the
 * attribute, though any collision is enough to trigger a warning.
 *
 * We avoid flagging these problems as errors because XFS treats names as a
 * sequence of arbitrary nonzero bytes.  While a Unicode collision is not
 * technically a filesystem corruption, we ought to say something if there's a
 * possibility for misleading a user.  Unquestionably bad things (direction
 * overrides, control characters, names that normalize to the same string)
 * produce warnings, whereas potentially confusable names produce
 * informational messages.
 *
 * The skeleton algorithm is detailed in section 4 ("Confusable Detection") of
 * the Unicode technical standard #39.  First we normalize the name, then we
 * substitute code points according to the confusable code point table, then
 * normalize again.
 *
 * We take the extra step of removing non-identifier code points such as
 * formatting characters, control characters, zero width characters, etc.
 * from the skeleton so that we can complain about names that are confusable
 * due to invisible control characters.
 *
 * In other words, skel = remove_invisible(nfd(remap_confusables(nfd(name)))).
 */

typedef uint16_t __bitwise	badname_t;

struct name_entry {
	struct name_entry	*next;

	/* NFKC normalized name */
	UChar			*normstr;

	/* Unicode skeletonized name */
	UChar			*skelstr;

	/* Lengths for normstr and skelstr */
	int32_t			normstrlen;
	int32_t			skelstrlen;

	xfs_ino_t		ino;

	/* Everything that we don't like about this name. */
	badname_t		badflags;

	/* Raw dirent name */
	uint16_t		namelen;
	char			name[0];
};
#define NAME_ENTRY_SZ(nl)	(sizeof(struct name_entry) + 1 + \
				 (nl * sizeof(uint8_t)))

struct unicrash {
	struct scrub_ctx	*ctx;
	USpoofChecker		*spoof;
	const UNormalizer2	*nfkc;
	const UNormalizer2	*nfc;
	bool			compare_ino;
	bool			is_only_root_writeable;
	size_t			nr_buckets;
	struct name_entry	*buckets[0];
};
#define UNICRASH_SZ(nr)		(sizeof(struct unicrash) + \
				 (nr * sizeof(struct name_entry *)))

/* Things to complain about in Unicode naming. */

/* Everything is ok */
#define UNICRASH_OK		((__force badname_t)0)

/*
 * Multiple names resolve to the same normalized string and therefore render
 * identically.
 */
#define UNICRASH_NOT_UNIQUE	((__force badname_t)(1U << 0))

/* Name contains directional overrides. */
#define UNICRASH_BIDI_OVERRIDE	((__force badname_t)(1U << 1))

/* Name mixes left-to-right and right-to-left characters. */
#define UNICRASH_BIDI_MIXED	((__force badname_t)(1U << 2))

/* Control characters in name. */
#define UNICRASH_CONTROL_CHAR	((__force badname_t)(1U << 3))

/* Invisible characters.  Only a problem if we have collisions. */
#define UNICRASH_INVISIBLE	((__force badname_t)(1U << 4))

/* Multiple names resolve to the same skeleton string. */
#define UNICRASH_CONFUSABLE	((__force badname_t)(1U << 5))

/* Possible phony file extension. */
#define UNICRASH_PHONY_EXTENSION ((__force badname_t)(1U << 6))

/* FULL STOP (aka period), 0x2E */
#define UCHAR_PERIOD		((UChar32)'.')

/*
 * We only care about validating utf8 collisions if the underlying
 * system configuration says we're using utf8.  If the language
 * specifier string used to output messages has ".UTF-8" somewhere in
 * its name, then we conclude utf8 is in use.  Otherwise, no checking is
 * performed.
 *
 * Most modern Linux systems default to utf8, so the only time this
 * check will return false is if the administrator configured things
 * this way or if things are so messed up there is no locale data at
 * all.
 */
#define UTF8_STR		".UTF-8"
#define UTF8_STRLEN		(sizeof(UTF8_STR) - 1)
static bool
is_utf8_locale(void)
{
	const char		*msg_locale;
	static int		answer = -1;

	if (answer != -1)
		return answer;

	msg_locale = setlocale(LC_MESSAGES, NULL);
	if (msg_locale == NULL)
		return false;

	if (strstr(msg_locale, UTF8_STR) != NULL)
		answer = 1;
	else
		answer = 0;
	return answer;
}

/*
 * Remove control/formatting characters from this string and return its new
 * length.  UChar32 is required for U16_NEXT, despite the name.
 */
static int32_t
remove_ignorable(
	UChar		*ustr,
	int32_t		ustrlen)
{
	UChar32		uchr;
	int32_t		src, dest;

	for (src = 0, dest = 0; src < ustrlen; dest = src) {
		U16_NEXT(ustr, src, ustrlen, uchr);
		if (!u_isIDIgnorable(uchr))
			continue;
		memmove(&ustr[dest], &ustr[src],
				(ustrlen - src + 1) * sizeof(UChar));
		ustrlen -= (src - dest);
		src = dest;
	}

	return dest;
}

/*
 * Certain unicode codepoints are formatting hints that are not themselves
 * supposed to be rendered by a display system.  These codepoints can be
 * encoded in file names to try to confuse users.
 *
 * Download https://www.unicode.org/Public/UCD/latest/ucd/UnicodeData.txt and
 * $ grep -E '(zero width|invisible|joiner|application)' -i UnicodeData.txt
 */
static inline bool is_nonrendering(UChar32 uchr)
{
	switch (uchr) {
	case 0x034F:	/* combining grapheme joiner */
	case 0x200B:	/* zero width space */
	case 0x200C:	/* zero width non-joiner */
	case 0x200D:	/* zero width joiner */
	case 0x2028:	/* line separator */
	case 0x2029:	/* paragraph separator */
	case 0x2060:	/* word joiner */
	case 0x2061:	/* function application */
	case 0x2062:	/* invisible times (multiply) */
	case 0x2063:	/* invisible separator (comma) */
	case 0x2064:	/* invisible plus (addition) */
	case 0x2D7F:	/* tifinagh consonant joiner */
	case 0xFEFF:	/* zero width non breaking space */
		return true;
	}

	return false;
}

/*
 * Decide if this unicode codepoint looks similar enough to a period (".")
 * to fool users into thinking that any subsequent alphanumeric sequence is
 * the file extension.  Most of the fullstop characters do not do this.
 *
 * $ grep -i 'full stop' UnicodeData.txt
 */
static inline bool is_fullstop_lookalike(UChar32 uchr)
{
	switch (uchr) {
	case 0x0701:	/* syriac supralinear full stop */
	case 0x0702:	/* syriac sublinear full stop */
	case 0x2024:	/* one dot leader */
	case 0xA4F8:	/* lisu letter tone mya ti */
	case 0xFE52:	/* small full stop */
	case 0xFF61:	/* haflwidth ideographic full stop */
	case 0xFF0E:	/* fullwidth full stop */
		return true;
	}

	return false;
}

/* How many UChar do we need to fit a full UChar32 codepoint? */
#define UCHAR_PER_UCHAR32	2

/* Format this UChar32 into a UChar buffer. */
static inline int32_t
uchar32_to_uchar(
	UChar32		uchr,
	UChar		*buf)
{
	int32_t		i = 0;
	bool		err = false;

	U16_APPEND(buf, i, UCHAR_PER_UCHAR32, uchr, err);
	if (err)
		return 0;
	return i;
}

/* Extract a single UChar32 code point from this UChar string. */
static inline UChar32
uchar_to_uchar32(
	UChar		*buf,
	int32_t		buflen)
{
	UChar32		ret;
	int32_t		i = 0;

	U16_NEXT(buf, i, buflen, ret);
	return ret;
}

/*
 * For characters that are not themselves a full stop (0x2E), let's see if the
 * compatibility normalization (NFKC) will turn it into a full stop.  If so,
 * then this could be the start of a phony file extension.
 */
static bool
is_period_lookalike(
	struct unicrash	*uc,
	UChar32		uchr)
{
	UChar		uchrstr[UCHAR_PER_UCHAR32];
	UChar		nfkcstr[UCHAR_PER_UCHAR32];
	int32_t		uchrstrlen, nfkcstrlen;
	UChar32		nfkc_uchr;
	UErrorCode	uerr = U_ZERO_ERROR;

	if (uchr == UCHAR_PERIOD)
		return false;

	uchrstrlen = uchar32_to_uchar(uchr, uchrstr);
	if (!uchrstrlen)
		return false;

	/*
	 * Normalize the UChar string to NFKC form, which does all the
	 * compatibility transformations.
	 */
	nfkcstrlen = unorm2_normalize(uc->nfkc, uchrstr, uchrstrlen, NULL,
			0, &uerr);
	if (uerr == U_BUFFER_OVERFLOW_ERROR)
		return false;

	uerr = U_ZERO_ERROR;
	unorm2_normalize(uc->nfkc, uchrstr, uchrstrlen, nfkcstr, nfkcstrlen,
			&uerr);
	if (U_FAILURE(uerr))
		return false;

	nfkc_uchr = uchar_to_uchar32(nfkcstr, nfkcstrlen);
	return nfkc_uchr == UCHAR_PERIOD;
}

/*
 * Detect directory entry names that contain deceptive sequences that look like
 * file extensions but are not.  This we define as a sequence that begins with
 * a code point that renders like a period ("full stop" in unicode parlance)
 * but is not actually a period, followed by any number of alphanumeric code
 * points or a period, all the way to the end.
 *
 * The 3cx attack used a zip file containing an executable file named "job
 * offer․pdf".  Note that the dot mark in the extension is /not/ a period but
 * the Unicode codepoint "leader dot".  The file was also marked executable
 * inside the zip file, which meant that naïve file explorers could inflate
 * the file and restore the execute bit.  If a user double-clicked on the file,
 * the binary would open a decoy pdf while infecting the system.
 *
 * For this check, we need to normalize with canonical (and not compatibility)
 * decomposition, because compatibility mode will turn certain code points
 * (e.g. one dot leader, 0x2024) into actual periods (0x2e).  The NFC
 * composition is not needed after this, so we save some memory by keeping this
 * a separate function from name_entry_examine.
 */
static badname_t
name_entry_phony_extension(
	struct unicrash	*uc,
	const UChar	*unistr,
	int32_t		unistrlen)
{
	UCharIterator	uiter;
	UChar		*nfcstr;
	int32_t		nfcstrlen;
	UChar32		uchr;
	bool		maybe_phony_extension = false;
	badname_t	ret = UNICRASH_OK;
	UErrorCode	uerr = U_ZERO_ERROR;

	/* Normalize with NFC. */
	nfcstrlen = unorm2_normalize(uc->nfc, unistr, unistrlen, NULL,
			0, &uerr);
	if (uerr != U_BUFFER_OVERFLOW_ERROR || nfcstrlen < 0)
		return ret;
	uerr = U_ZERO_ERROR;
	nfcstr = calloc(nfcstrlen + 1, sizeof(UChar));
	if (!nfcstr)
		return ret;
	unorm2_normalize(uc->nfc, unistr, unistrlen, nfcstr, nfcstrlen,
			&uerr);
	if (U_FAILURE(uerr))
		goto out_nfcstr;

	/* Examine the NFC normalized string... */
	uiter_setString(&uiter, nfcstr, nfcstrlen);
	while ((uchr = uiter_next32(&uiter)) != U_SENTINEL) {
		/*
		 * If this *looks* like, but is not, a full stop (0x2E), this
		 * could be the start of a phony file extension.
		 */
		if (is_period_lookalike(uc, uchr)) {
			maybe_phony_extension = true;
			continue;
		}

		if (is_fullstop_lookalike(uchr)) {
			/*
			 * The normalizer above should catch most of these
			 * codepoints that look like periods, but record the
			 * ones known to have been used in attacks.
			 */
			maybe_phony_extension = true;
		} else if (uchr == UCHAR_PERIOD) {
			/*
			 * Due to the propensity of file explorers to obscure
			 * file extensions in the name of "user friendliness",
			 * this classifier ignores periods.
			 */
		} else {
			/*
			 * File extensions (as far as the author knows) tend
			 * only to use ascii alphanumerics.
			 */
			if (maybe_phony_extension &&
			    !u_isalnum(uchr) && !is_nonrendering(uchr))
				maybe_phony_extension = false;
		}
	}
	if (maybe_phony_extension)
		ret |= UNICRASH_PHONY_EXTENSION;

out_nfcstr:
	free(nfcstr);
	return ret;
}

/*
 * Generate normalized form and skeleton of the name.  If this fails, just
 * forget everything and return false; this is an advisory checker.
 */
static bool
name_entry_compute_checknames(
	struct unicrash		*uc,
	struct name_entry	*entry)
{
	UChar			*normstr;
	UChar			*unistr;
	UChar			*skelstr;
	int32_t			normstrlen;
	int32_t			unistrlen;
	int32_t			skelstrlen;
	UErrorCode		uerr = U_ZERO_ERROR;

	/* Convert bytestr to unistr for normalization */
	u_strFromUTF8(NULL, 0, &unistrlen, entry->name, entry->namelen, &uerr);
	if (uerr != U_BUFFER_OVERFLOW_ERROR || unistrlen < 0)
		return false;
	uerr = U_ZERO_ERROR;
	unistr = calloc(unistrlen + 1, sizeof(UChar));
	if (!unistr)
		return false;
	u_strFromUTF8(unistr, unistrlen, NULL, entry->name, entry->namelen,
			&uerr);
	if (U_FAILURE(uerr))
		goto out_unistr;

	/* Normalize the string. */
	normstrlen = unorm2_normalize(uc->nfkc, unistr, unistrlen, NULL,
			0, &uerr);
	if (uerr != U_BUFFER_OVERFLOW_ERROR || normstrlen < 0)
		goto out_unistr;
	uerr = U_ZERO_ERROR;
	normstr = calloc(normstrlen + 1, sizeof(UChar));
	if (!normstr)
		goto out_unistr;
	unorm2_normalize(uc->nfkc, unistr, unistrlen, normstr, normstrlen,
			&uerr);
	if (U_FAILURE(uerr))
		goto out_normstr;

	/* Compute skeleton. */
	skelstrlen = uspoof_getSkeleton(uc->spoof, 0, unistr, unistrlen, NULL,
			0, &uerr);
	if (uerr != U_BUFFER_OVERFLOW_ERROR || skelstrlen < 0)
		goto out_normstr;
	uerr = U_ZERO_ERROR;
	skelstr = calloc(skelstrlen + 1, sizeof(UChar));
	if (!skelstr)
		goto out_normstr;
	uspoof_getSkeleton(uc->spoof, 0, unistr, unistrlen, skelstr, skelstrlen,
			&uerr);
	if (U_FAILURE(uerr))
		goto out_skelstr;

	skelstrlen = remove_ignorable(skelstr, skelstrlen);

	/* Check for deceptive file extensions in directory entry names. */
	if (entry->ino)
		entry->badflags |= name_entry_phony_extension(uc, unistr,
						unistrlen);

	entry->skelstr = skelstr;
	entry->skelstrlen = skelstrlen;
	entry->normstr = normstr;
	entry->normstrlen = normstrlen;
	free(unistr);
	return true;

out_skelstr:
	free(skelstr);
out_normstr:
	free(normstr);
out_unistr:
	free(unistr);
	return false;
}

/*
 * Check a name for suspicious elements that have appeared in filename
 * spoofing attacks.  This includes names that mixed directions or contain
 * direction overrides control characters, both of which have appeared in
 * filename spoofing attacks.
 */
static unsigned int
name_entry_examine(
	const struct name_entry	*entry)
{
	UCharIterator		uiter;
	UChar32			uchr;
	uint8_t			mask = 0;
	unsigned int		ret = 0;

	uiter_setString(&uiter, entry->normstr, entry->normstrlen);
	while ((uchr = uiter_next32(&uiter)) != U_SENTINEL) {
		/* characters are invisible */
		if (is_nonrendering(uchr))
			ret |= UNICRASH_INVISIBLE;

		/*
		 * Warn about control characters in filenames except for zero
		 * width joiners because those are used to construct compound
		 * emoji and glyphs in various languages.  ZWJ is already
		 * covered by UNICRASH_INVISIBLE, so we can detect its use in
		 * confusing names.
		 */
		if (uchr != 0x200D && u_iscntrl(uchr))
			ret |= UNICRASH_CONTROL_CHAR;

		switch (u_charDirection(uchr)) {
		case U_LEFT_TO_RIGHT:
			mask |= 0x01;
			break;
		case U_RIGHT_TO_LEFT:
			mask |= 0x02;
			break;
		case U_RIGHT_TO_LEFT_OVERRIDE:
			ret |= UNICRASH_BIDI_OVERRIDE;
			break;
		case U_LEFT_TO_RIGHT_OVERRIDE:
			ret |= UNICRASH_BIDI_OVERRIDE;
			break;
		default:
			break;
		}
	}

	/* mixing left-to-right and right-to-left chars */
	if (mask == 0x3)
		ret |= UNICRASH_BIDI_MIXED;
	return ret;
}

/* Create a new name entry, returns false if we could not succeed. */
static bool
name_entry_create(
	struct unicrash		*uc,
	const char		*name,
	xfs_ino_t		ino,
	struct name_entry	**entry)
{
	struct name_entry	*new_entry;
	size_t			namelen = strlen(name);

	/* should never happen */
	if (namelen > UINT16_MAX) {
		ASSERT(namelen <= UINT16_MAX);
		return false;
	}

	/* Create new entry */
	new_entry = calloc(NAME_ENTRY_SZ(namelen), 1);
	if (!new_entry)
		return false;
	new_entry->next = NULL;
	new_entry->ino = ino;
	memcpy(new_entry->name, name, namelen);
	new_entry->name[namelen] = 0;
	new_entry->namelen = namelen;

	/* Normalize/skeletonize name to find collisions. */
	if (!name_entry_compute_checknames(uc, new_entry))
		goto out;

	new_entry->badflags |= name_entry_examine(new_entry);
	*entry = new_entry;
	return true;

out:
	free(new_entry);
	return false;
}

/* Free a name entry */
static void
name_entry_free(
	struct name_entry	*entry)
{
	free(entry->normstr);
	free(entry->skelstr);
	free(entry);
}

/* Adapt the dirhash function from libxfs, avoid linking with libxfs. */

#define rol32(x, y)		(((x) << (y)) | ((x) >> (32 - (y))))

/*
 * Implement a simple hash on a character string.
 * Rotate the hash value by 7 bits, then XOR each character in.
 * This is implemented with some source-level loop unrolling.
 */
static xfs_dahash_t
name_entry_hash(
	struct name_entry	*entry)
{
	uint8_t			*name;
	size_t			namelen;
	xfs_dahash_t		hash;

	name = (uint8_t *)entry->skelstr;
	namelen = entry->skelstrlen * sizeof(UChar);

	/*
	 * Do four characters at a time as long as we can.
	 */
	for (hash = 0; namelen >= 4; namelen -= 4, name += 4)
		hash = (name[0] << 21) ^ (name[1] << 14) ^ (name[2] << 7) ^
		       (name[3] << 0) ^ rol32(hash, 7 * 4);

	/*
	 * Now do the rest of the characters.
	 */
	switch (namelen) {
	case 3:
		return (name[0] << 14) ^ (name[1] << 7) ^ (name[2] << 0) ^
		       rol32(hash, 7 * 3);
	case 2:
		return (name[0] << 7) ^ (name[1] << 0) ^ rol32(hash, 7 * 2);
	case 1:
		return (name[0] << 0) ^ rol32(hash, 7 * 1);
	default: /* case 0: */
		return hash;
	}
}

/* Initialize the collision detector. */
static int
unicrash_init(
	struct unicrash		**ucp,
	struct scrub_ctx	*ctx,
	bool			compare_ino,
	size_t			nr_buckets,
	bool			is_only_root_writeable)
{
	struct unicrash		*p;
	UErrorCode		uerr = U_ZERO_ERROR;

	if (!is_utf8_locale()) {
		*ucp = NULL;
		return 0;
	}

	if (nr_buckets > 65536)
		nr_buckets = 65536;
	else if (nr_buckets < 16)
		nr_buckets = 16;

	p = calloc(1, UNICRASH_SZ(nr_buckets));
	if (!p)
		return errno;
	p->ctx = ctx;
	p->nr_buckets = nr_buckets;
	p->compare_ino = compare_ino;
	p->nfkc = unorm2_getNFKCInstance(&uerr);
	if (U_FAILURE(uerr))
		goto out_free;
	p->nfc = unorm2_getNFCInstance(&uerr);
	if (U_FAILURE(uerr))
		goto out_free;
	p->spoof = uspoof_open(&uerr);
	if (U_FAILURE(uerr))
		goto out_free;
	uspoof_setChecks(p->spoof, USPOOF_ALL_CHECKS, &uerr);
	if (U_FAILURE(uerr))
		goto out_spoof;
	p->is_only_root_writeable = is_only_root_writeable;
	*ucp = p;

	return 0;
out_spoof:
	uspoof_close(p->spoof);
out_free:
	free(p);
	return ENOMEM;
}

/*
 * Is this inode owned by root and not writable by others?  If so, skip
 * even the informational messages, because this was put in place by the
 * administrator.
 */
static bool
is_only_root_writable(
	struct xfs_bulkstat	*bstat)
{
	if (bstat->bs_uid != 0 || bstat->bs_gid != 0)
		return false;
	return !(bstat->bs_mode & S_IWOTH);
}

/* Initialize the collision detector for a directory. */
int
unicrash_dir_init(
	struct unicrash		**ucp,
	struct scrub_ctx	*ctx,
	struct xfs_bulkstat	*bstat)
{
	/*
	 * Assume 64 bytes per dentry, clamp buckets between 16 and 64k.
	 * Same general idea as dir_hash_init in xfs_repair.
	 */
	return unicrash_init(ucp, ctx, true, bstat->bs_size / 64,
			is_only_root_writable(bstat));
}

/* Initialize the collision detector for an extended attribute. */
int
unicrash_xattr_init(
	struct unicrash		**ucp,
	struct scrub_ctx	*ctx,
	struct xfs_bulkstat	*bstat)
{
	/* Assume 16 attributes per extent for lack of a better idea. */
	return unicrash_init(ucp, ctx, false, 16 * (1 + bstat->bs_aextents),
			is_only_root_writable(bstat));
}

/* Initialize the collision detector for a filesystem label. */
int
unicrash_fs_label_init(
	struct unicrash		**ucp,
	struct scrub_ctx	*ctx)
{
	return unicrash_init(ucp, ctx, false, 16, true);
}

/* Free the crash detector. */
void
unicrash_free(
	struct unicrash		*uc)
{
	struct name_entry	*ne;
	struct name_entry	*x;
	size_t			i;

	if (!uc)
		return;

	uspoof_close(uc->spoof);
	for (i = 0; i < uc->nr_buckets; i++) {
		for (ne = uc->buckets[i]; ne != NULL; ne = x) {
			x = ne->next;
			name_entry_free(ne);
		}
	}
	free(uc);
}

/* Complain about Unicode problems. */
static void
unicrash_complain(
	struct unicrash		*uc,
	struct descr		*dsc,
	const char		*what,
	struct name_entry	*entry,
	badname_t		badflags,
	struct name_entry	*dup_entry)
{
	char			*bad1 = NULL;
	char			*bad2 = NULL;

	bad1 = string_escape(entry->name);
	if (dup_entry)
		bad2 = string_escape(dup_entry->name);

	/*
	 * Most filechooser UIs do not look for bidirectional overrides when
	 * they render names.  This can result in misleading name presentation
	 * that makes "hig<rtl>gnp.sh" render like "highs.png".
	 */
	if (badflags & UNICRASH_BIDI_OVERRIDE) {
		str_warn(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s contains suspicious text direction overrides."),
				bad1, what);
		goto out;
	}

	/*
	 * Two names that normalize to the same string will render
	 * identically even though the filesystem considers them unique
	 * names.  "cafe\xcc\x81" and "caf\xc3\xa9" have different byte
	 * sequences, but they both appear as "café".
	 */
	if (badflags & UNICRASH_NOT_UNIQUE) {
		str_warn(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s renders identically to \"%s\"."),
				bad1, what, bad2);
		goto out;
	}

	/*
	 * If a name contains invisible/nonprinting characters and can be
	 * confused with another name as a result, we should complain.
	 * "moo<zerowidthspace>cow" and "moocow" are misleading.
	 */
	if ((badflags & UNICRASH_INVISIBLE) &&
	    (badflags & UNICRASH_CONFUSABLE)) {
		str_warn(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s could be confused with '%s' due to invisible characters."),
				bad1, what, bad2);
		goto out;
	}

	/*
	 * Fake looking file extensions have tricked Linux users into thinking
	 * that an executable is actually a pdf.  See Lazarus 3cx attack.
	 */
	if (badflags & UNICRASH_PHONY_EXTENSION) {
		str_warn(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s contains a possibly deceptive file extension."),
				bad1, what);
		goto out;
	}

	/*
	 * Unfiltered control characters can mess up your terminal and render
	 * invisibly in filechooser UIs.
	 */
	if (badflags & UNICRASH_CONTROL_CHAR) {
		str_warn(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s contains control characters."),
				bad1, what);
		goto out;
	}

	/*
	 * Skip the informational messages if the inode owning the name is
	 * only writeable by root, because those files were put there by the
	 * sysadmin.  Also skip names less than four letters long because
	 * there's a much higher chance of collisions with short names.
	 */
	if (!verbose && (uc->is_only_root_writeable || entry->namelen < 4))
		goto out;

	/*
	 * It's not considered good practice (says Unicode) to mix LTR
	 * characters with RTL characters.  The mere presence of different
	 * bidirectional characters isn't enough to trip up software, so don't
	 * warn about this too loudly.
	 */
	if (badflags & UNICRASH_BIDI_MIXED) {
		str_info(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s mixes bidirectional characters."),
				bad1, what);
		goto out;
	}

	/*
	 * We'll note if two names could be confusable with each other, but
	 * whether or not the user will actually confuse them is dependent
	 * on the rendering system and the typefaces in use.  Maybe "foo.1"
	 * and "moo.l" look the same, maybe they do not.
	 */
	if (badflags & UNICRASH_CONFUSABLE) {
		str_info(uc->ctx, descr_render(dsc),
_("Unicode name \"%s\" in %s could be confused with \"%s\"."),
				bad1, what, bad2);
	}

out:
	free(bad1);
	free(bad2);
}

/*
 * Try to add a name -> ino entry to the collision detector.  The name
 * must be skeletonized according to Unicode TR39 to detect names that
 * could be visually confused with each other.
 */
static badname_t
unicrash_add(
	struct unicrash		*uc,
	struct name_entry	**new_entryp,
	struct name_entry	**existing_entry)
{
	struct name_entry	*new_entry = *new_entryp;
	struct name_entry	*entry;
	size_t			bucket;
	xfs_dahash_t		hash;
	badname_t		badflags = new_entry->badflags;

	/* Store name in hashtable. */
	hash = name_entry_hash(new_entry);
	bucket = hash % uc->nr_buckets;
	entry = uc->buckets[bucket];
	new_entry->next = entry;
	uc->buckets[bucket] = new_entry;

	while (entry != NULL) {
		/*
		 * If we see the same byte sequence then someone's modifying
		 * the namespace while we're scanning it.  Update the existing
		 * entry's inode mapping and erase the new entry from existence.
		 */
		if (new_entry->namelen == entry->namelen &&
		    !memcmp(new_entry->name, entry->name, entry->namelen)) {
			entry->ino = new_entry->ino;
			uc->buckets[bucket] = new_entry->next;
			name_entry_free(new_entry);
			*new_entryp = NULL;
			return 0;
		}

		/* Same normalization? */
		if (new_entry->normstrlen == entry->normstrlen &&
		    !u_strcmp(new_entry->normstr, entry->normstr) &&
		    (uc->compare_ino ? entry->ino != new_entry->ino : true)) {
			badflags |= UNICRASH_NOT_UNIQUE;
			*existing_entry = entry;
			break;
		}

		/* Confusable? */
		if (new_entry->skelstrlen == entry->skelstrlen &&
		    !u_strcmp(new_entry->skelstr, entry->skelstr) &&
		    (uc->compare_ino ? entry->ino != new_entry->ino : true)) {
			badflags |= UNICRASH_CONFUSABLE;
			*existing_entry = entry;
			break;
		}
		entry = entry->next;
	}

	return badflags;
}

/* Check a name for unicode normalization problems or collisions. */
static int
__unicrash_check_name(
	struct unicrash		*uc,
	struct descr		*dsc,
	const char		*namedescr,
	const char		*name,
	xfs_ino_t		ino)
{
	struct name_entry	*dup_entry = NULL;
	struct name_entry	*new_entry = NULL;
	badname_t		badflags;

	/* If we can't create entry data, just skip it. */
	if (!name_entry_create(uc, name, ino, &new_entry))
		return 0;

	badflags = unicrash_add(uc, &new_entry, &dup_entry);
	if (new_entry && badflags != UNICRASH_OK)
		unicrash_complain(uc, dsc, namedescr, new_entry, badflags,
				dup_entry);

	return 0;
}

/*
 * Check a directory entry for unicode normalization problems or collisions.
 * If errors occur, this function will log them and return nonzero.
 */
int
unicrash_check_dir_name(
	struct unicrash		*uc,
	struct descr		*dsc,
	struct dirent		*dentry)
{
	if (!uc)
		return 0;
	return __unicrash_check_name(uc, dsc, _("directory"),
			dentry->d_name, dentry->d_ino);
}

/*
 * Check an extended attribute name for unicode normalization problems
 * or collisions.  If errors occur, this function will log them and return
 * nonzero.
 */
int
unicrash_check_xattr_name(
	struct unicrash		*uc,
	struct descr		*dsc,
	const char		*attrname)
{
	if (!uc)
		return 0;
	return __unicrash_check_name(uc, dsc, _("extended attribute"),
			attrname, 0);
}

/*
 * Check the fs label for unicode normalization problems or misleading bits.
 * If errors occur, this function will log them and return nonzero.
 */
int
unicrash_check_fs_label(
	struct unicrash		*uc,
	struct descr		*dsc,
	const char		*label)
{
	if (!uc)
		return 0;
	return __unicrash_check_name(uc, dsc, _("filesystem label"),
			label, 0);
}

/* Dump a unicode code point and its properties. */
static inline void dump_uchar32(UChar32 c)
{
	UChar		uchrstr[UCHAR_PER_UCHAR32];
	const char	*descr;
	char		buf[16];
	int32_t		uchrstrlen, buflen;
	UProperty	p;
	UErrorCode	uerr = U_ZERO_ERROR;

	printf("Unicode point 0x%x:", c);

	/* Convert UChar32 to UTF8 representation. */
	uchrstrlen = uchar32_to_uchar(c, uchrstr);
	if (!uchrstrlen)
		return;

	u_strToUTF8(buf, sizeof(buf), &buflen, uchrstr, uchrstrlen, &uerr);
	if (!U_FAILURE(uerr) && buflen > 0) {
		int32_t	i;

		printf(" \"");
		for (i = 0; i < buflen; i++)
			printf("\\x%02x", buf[i]);
		printf("\"");
	}
	printf("\n");

	for (p = 0; p < UCHAR_BINARY_LIMIT; p++) {
		int	has;

		descr = u_getPropertyName(p, U_LONG_PROPERTY_NAME);
		if (!descr)
			descr = u_getPropertyName(p, U_SHORT_PROPERTY_NAME);

		has = u_hasBinaryProperty(c, p) ? 1 : 0;
		if (descr) {
			printf("  %s(%u) = %d\n", descr, p, has);
		} else {
			printf("  ?(%u) = %d\n", p, has);
		}
	}
}

/* Load libicu and initialize it. */
bool
unicrash_load(void)
{
	char		*dbgstr;
	UChar32		uchr;
	UErrorCode	uerr = U_ZERO_ERROR;

	u_init(&uerr);
	if (U_FAILURE(uerr))
		return true;

	dbgstr = getenv("XFS_SCRUB_DUMP_CHAR");
	if (dbgstr) {
		uchr = strtol(dbgstr, NULL, 0);
		dump_uchar32(uchr);
	}
	return false;
}

/* Unload libicu once we're done with it. */
void
unicrash_unload(void)
{
	u_cleanup();
}
