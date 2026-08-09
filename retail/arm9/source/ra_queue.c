/*
    The unlock queue: reading it, signing what is in it, and writing back what is still owed.

    Everything here is pure. No sockets, no FILE*, no hardware -- the file is handed in as bytes and
    handed back as bytes, so tools/ra_launcher_test.c can drive all of it on a PC. That split is the
    same one ra_patch.c makes and it is not stylistic: a wrong signature comes back from
    RetroAchievements as a generic refusal, with nothing in the reply that says "your md5 is wrong",
    so it is the single most expensive thing in this feature to debug from a console. Pinning it
    against digests computed by coreutils moves that debugging to a PC where it costs seconds.

    Why a file at all: the cardengine has no network. It runs inside the game, after the radio has
    been torn down, so an achievement earned while playing cannot be reported when it happens. It is
    written to sd:/ra_unlocks.txt and sent by the *next* boot's launcher, which is why an unlock is
    late rather than lost. See RA_QUEUE_PATH for the two constraints that pick the format.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <string.h>

#include "rhash/md5.h"

/*
    Read the file.

    Deliberately the most forgiving parser in this project. Every non-digit is a separator, NUL
    padding included, so the fixed 16-byte records the cardengine will write and a line a human typed
    into a text editor parse identically -- and that equivalence is what lets the sending half be
    tested before the writing half exists. `#` to end of line is a comment, so a note left in the
    file cannot turn into a spurious unlock.

    What it refuses rather than mangles: anything that does not fit in u32. A truncated id is a
    *different achievement*, so an overflowing run of digits is dropped and counted. This is the same
    correction ra_patch.c and ra_take_id() needed -- clamping a number that is too long silently
    submits the wrong thing, which is worse than submitting nothing.

    Duplicates are collapsed. The cardengine should not write one twice, and if it does, awarding it
    twice is not the way to find out.
*/
void raQueueScan(raQueue* q, const char* text, int length) {
	int i = 0;

	memset(q, 0, sizeof(*q));

	while (i < length) {
		u32 value;
		int overflow;
		int digits;
		int j;
		int seen;

		if (text[i] == '#') {
			while (i < length && text[i] != '\n') {
				i++;
			}
			continue;
		}
		if (text[i] < '0' || text[i] > '9') {
			i++;
			continue;
		}

		value    = 0;
		overflow = 0;
		digits   = 0;
		while (i < length && text[i] >= '0' && text[i] <= '9') {
			const u32 digit = (u32)(text[i] - '0');

			/*
			    Refuse before the multiply rather than after: 0xFFFFFFFF/10 is the last value that
			    can be multiplied without wrapping, and checking afterwards means reading a number
			    that already wrapped.
			*/
			if (value > 0xFFFFFFFFu / 10u
			 || (value == 0xFFFFFFFFu / 10u && digit > 0xFFFFFFFFu % 10u)) {
				overflow = 1;
			}
			if (!overflow) {
				value = value * 10u + digit;
			}
			digits++;
			i++;
		}

		if (overflow || digits == 0 || value == 0) {
			/*
			    Zero is not an achievement id -- rcheevos refuses it outright
			    (RC_INVALID_STATE) -- and it is what NUL padding reads as if the padding were
			    ever mistaken for digits. Counting it keeps a file full of zeros from looking
			    like an empty one.
			*/
			q->dropped++;
			continue;
		}

		seen = 0;
		for (j = 0; j < q->count; j++) {
			if (q->ids[j] == value) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			continue;
		}

		if (q->count >= RA_QUEUE_MAX) {
			q->truncated++;
			continue;
		}
		q->ids[q->count++] = value;
	}
}

/*
    Write back what is still owed.

    Fixed-size records, and `out` is filled to exactly outSize with NULs after the last one, because
    the caller writes all of it over the file: the length must not change. See RA_QUEUE_PATH -- the
    cardengine can only write into clusters that already exist, and shortening the file could hand
    them back.

    Returns the number of records written, or -1 if the buffer is not the size the format needs. It
    refuses rather than writing a partial file: half a queue is indistinguishable from a whole one on
    the next boot.
*/
int raQueuePack(const raQueue* q, const u32* keep, int keepCount, char* out, int outSize) {
	int written = 0;
	int i;

	(void)q;

	if (outSize < RA_QUEUE_RECORD || (outSize % RA_QUEUE_RECORD) != 0) {
		return -1;
	}
	if (keepCount < 0 || keepCount > outSize / RA_QUEUE_RECORD) {
		return -1;
	}

	memset(out, 0, (size_t)outSize);

	for (i = 0; i < keepCount; i++) {
		char* record = out + (i * RA_QUEUE_RECORD);
		char  digits[11];
		int   n = 0;
		u32   value = keep[i];

		if (value == 0) {
			continue;
		}
		while (value && n < (int)sizeof(digits)) {
			digits[n++] = (char)('0' + (value % 10u));
			value /= 10u;
		}
		/*
		    A u32 is at most 10 digits and a record is 16 bytes, so this cannot fail -- but the
		    check is here rather than in a comment, because the record size is a tunable and the
		    failure it would cause is a silently mis-parsed id.
		*/
		if (n + 1 > RA_QUEUE_RECORD) {
			return -1;
		}
		{
			int k;

			for (k = 0; k < n; k++) {
				record[k] = digits[n - 1 - k];
			}
			record[n] = '\n';
		}
		written++;
	}
	return written;
}

/*
    v=<md5>, the parameter that turns "here is an id" into "here is an id from this account".

    The formula is not remembered, it is read: rcheevos'
    rc_api_init_award_achievement_request_hosted() in the vendored copy under
    retail/cardenginei/arm9_ra/rcheevos/ appends the id as decimal, the username, and the hardcore
    flag as "0" or "1", with no separators, and md5s that. The two extra fields it appends only exist
    when a delegated unlock sends `o=` (seconds since the unlock), which this client does not -- see
    the note in docs/retroachievements.md about unlock timestamps being the submission time.

    The username is the raw one. u= in the URL is percent-encoded and this is not, because the server
    hashes what it decoded; swapping them breaks exactly the accounts with a space or a plus in the
    name and no others.
*/
void raQueueSign(u32 id, const char* username, int hardcore, char* out) {
	static const char hex[] = "0123456789abcdef";
	md5_state_t       md5;
	md5_byte_t        digest[16];
	char              number[11];
	int               n = 0;
	int               i;
	u32               value = id;

	md5_init(&md5);

	if (value == 0) {
		number[n++] = '0';
	}
	while (value && n < (int)sizeof(number)) {
		number[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	for (i = 0; i < n; i++) {
		const char c = number[n - 1 - i];

		md5_append(&md5, (const md5_byte_t*)&c, 1);
	}

	md5_append(&md5, (const md5_byte_t*)username, (int)strlen(username));

	{
		const char flag = hardcore ? '1' : '0';

		md5_append(&md5, (const md5_byte_t*)&flag, 1);
	}

	md5_finish(&md5, digest);

	for (i = 0; i < 16; i++) {
		out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
		out[i * 2 + 1] = hex[digest[i] & 0xF];
	}
	out[32] = 0;
}

#endif /* RA_LAUNCHER_WIFI */
