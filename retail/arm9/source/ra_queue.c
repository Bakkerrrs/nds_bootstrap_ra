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
    A record's `YYYYMMDDhhmmss` stamp, as Unix seconds. Returns 0 for anything it will not vouch for,
    and 0 means "no stamp" everywhere downstream -- so a nonsense date degrades to sending the unlock
    without `o=` rather than dating it wrongly. Submitting a wrong time is worse than submitting none:
    the whole point of `o=` is to stop the server dating the unlock by when it was submitted.

    Its own function, and pure, because it is the one piece of this that is genuinely easy to get
    wrong -- leap years, month lengths, the epoch -- and the one the host suite can pin against dates
    computed by `date -u +%s`.

    Local time is treated as UTC. The console's RTC has no zone and neither does the DS, so the
    alternative is inventing one. It cancels out: the launcher's own clock is read the same way, and
    `o=` is a *difference* between the two, so a constant offset disappears from it entirely. What
    survives is the case where the user changes the console's clock between earning and submitting,
    which no encoding can defend against.
*/
u32 raQueueStampToUnix(const char* digits) {
	/* Days from 1970-01-01 to the first of each month, in a non-leap year. */
	static const u16 monthStart[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

	u32 field[6] = { 0, 0, 0, 0, 0, 0 };  /* year, month, day, hour, minute, second */
	u32 days;
	u32 y;
	int i;

	/*
	    Widths, in order: 4 for the year and 2 for everything after it. Anything that is not a digit
	    where a digit belongs fails the whole stamp rather than being skipped -- a partly-read date is
	    a wrong date.
	*/
	{
		static const u8 width[6] = { 4, 2, 2, 2, 2, 2 };
		int at = 0;

		for (i = 0; i < 6; i++) {
			int k;

			for (k = 0; k < (int)width[i]; k++) {
				const char c = digits[at++];

				if (c < '0' || c > '9') {
					return 0;
				}
				field[i] = field[i] * 10u + (u32)(c - '0');
			}
		}
	}

	/*
	    Ranges, and the year floor is the useful one: the DS RTC reports two digits and libnds adds
	    2000, so a console whose clock was never set reads as 2000 and is not a date anybody earned an
	    achievement at. 2001 is the floor rather than 1970 because no value below it can be real here.
	*/
	if (field[0] < 2001 || field[0] > 2099
	 || field[1] < 1 || field[1] > 12
	 || field[2] < 1 || field[2] > 31
	 || field[3] > 23 || field[4] > 59 || field[5] > 59) {
		return 0;
	}

	/* Whole years, with a leap day for each one divisible by 4 -- exact from 2001 to 2099. */
	days = 0;
	for (y = 1970; y < field[0]; y++) {
		days += ((y % 4u) == 0u && ((y % 100u) != 0u || (y % 400u) == 0u)) ? 366u : 365u;
	}
	days += monthStart[field[1] - 1];
	if (field[1] > 2 && (field[0] % 4u) == 0u && ((field[0] % 100u) != 0u || (field[0] % 400u) == 0u)) {
		days++;
	}
	days += field[2] - 1u;

	return ((days * 24u + field[3]) * 60u + field[4]) * 60u + field[5];
}

/*
    The inverse, writing `YYYYMMDDhhmmss` into fourteen bytes.

    It exists because an unlock whose request never got an answer is *kept*, and rewriting it without
    its stamp would throw away the earn time on exactly the records the queue exists to protect -- the
    ones being retried on a later boot, which are the ones where `o=` matters most.

    Being the exact inverse is the point, and the suite checks it as a round trip rather than against a
    second hand-written table: two conversions that agree with each other and with `date -u` are worth
    more than either one checked alone.
*/
void raQueueUnixToStamp(u32 when, char* out) {
	static const u8 monthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	u32 days = when / 86400u;
	u32 rem  = when % 86400u;
	u32 year = 1970;
	u32 month = 0;
	int i;

	for (;;) {
		const u32 length = ((year % 4u) == 0u && ((year % 100u) != 0u || (year % 400u) == 0u))
		                 ? 366u : 365u;

		if (days < length) {
			break;
		}
		days -= length;
		year++;
	}
	for (month = 0; month < 12; month++) {
		u32 length = monthDays[month];

		if (month == 1 && (year % 4u) == 0u && ((year % 100u) != 0u || (year % 400u) == 0u)) {
			length = 29;
		}
		if (days < length) {
			break;
		}
		days -= length;
	}

	{
		const u32 value[6] = { year, month + 1u, days + 1u, rem / 3600u, (rem / 60u) % 60u, rem % 60u };
		static const u8 width[6] = { 4, 2, 2, 2, 2, 2 };
		int at = 0;

		for (i = 0; i < 6; i++) {
			u32 v = value[i];
			int k;

			for (k = (int)width[i] - 1; k >= 0; k--) {
				out[at + k] = (char)('0' + (v % 10u));
				v /= 10u;
			}
			at += width[i];
		}
	}
}

/*
    Read the file.

    Deliberately the most forgiving parser in this project. Every non-digit is a separator, NUL
    padding included, so the fixed records the cardengine writes and a line a human typed into a text
    editor parse identically -- and that equivalence is what lets the sending half be tested before
    the writing half exists. `#` to end of line is a comment, so a note left in the file cannot turn
    into a spurious unlock.

    The one place it is *not* forgiving is the stamp, and it has to be: a tab straight after an id
    introduces `YYYYMMDDhhmmss`, and those fourteen digits are a date rather than another unlock. Read
    with the old rule they would have become a second, enormous id -- which is exactly why the
    delimiter is a character the old format could never contain. Anything else after the id, tab or
    not, is a separator as before.

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
		u32 stamp;
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

		/*
		    A tab here means the next fourteen characters are this record's stamp. Consumed whether or
		    not it turns out to be a date the converter will vouch for, because either way those digits
		    are not an unlock id and must not be read as one.
		*/
		stamp = 0;
		if (i < length && text[i] == '\t') {
			const int start = ++i;

			while (i < length && text[i] >= '0' && text[i] <= '9') {
				i++;
			}
			/*
			    Exactly RA_QUEUE_STAMP digits or it is not a stamp. Consuming the run either way is
			    the part that matters: those digits are a date the writer meant, so reading them as
			    an achievement id would invent an unlock out of a timestamp.

			    Bounded by the digit run rather than by a fixed fourteen bytes on purpose. A stamp cut
			    short -- by the end of the buffer, or by a hand-edited line -- would otherwise have
			    fourteen bytes counted off past its end, and in a file typed by hand that swallows the
			    *next* record's id.
			*/
			if (i - start == RA_QUEUE_STAMP) {
				stamp = raQueueStampToUnix(text + start);
			}
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
		q->times[q->count] = stamp;
		if (stamp) {
			q->stamped++;
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
int raQueuePack(const raQueue* q, const u32* keep, const u32* keepTimes, int keepCount,
                char* out, int outSize) {
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
		int   length;
		u32   value = keep[i];
		const u32 when = keepTimes ? keepTimes[i] : 0;

		if (value == 0) {
			continue;
		}
		while (value && n < (int)sizeof(digits)) {
			digits[n++] = (char)('0' + (value % 10u));
			value /= 10u;
		}
		/*
		    A u32 is at most 10 digits and a stamped record is 10 + 1 + 14 + 1 = 26 of the 32 a record
		    holds, so this cannot fail -- but the check is here rather than in a comment, because the
		    record size is a tunable and the failure it would cause is a silently mis-parsed id.
		*/
		length = n + 1 + (when ? 1 + RA_QUEUE_STAMP : 0);
		if (length > RA_QUEUE_RECORD) {
			return -1;
		}
		{
			int k;

			for (k = 0; k < n; k++) {
				record[k] = digits[n - 1 - k];
			}
			if (when) {
				record[n] = '\t';
				raQueueUnixToStamp(when, record + n + 1);
				record[n + 1 + RA_QUEUE_STAMP] = '\n';
			} else {
				record[n] = '\n';
			}
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
    flag as "0" or "1", with no separators, and md5s that.

    **And when `o=` is sent it appends two more fields: the id a second time, then the seconds.**
    rcheevos' own comment says why the id is repeated -- *"second achievement id is needed by delegated
    unlock. including it here allows overloading the hash generating code on the server"*. This is the
    trap in the whole timestamp change: `o=` in the URL without those two extra fields in the digest is
    a signature the server rejects, and it rejects it as a generic refusal that says nothing about
    which half was wrong. The two forms are kept in one function, switched by `seconds`, so the URL and
    the digest cannot disagree.

    `seconds == 0` means no `o=` at all, matching rcheevos: it writes the parameter only when the value
    is non-zero, so an unlock submitted inside the same second signs and sends exactly as before.

    The username is the raw one. u= in the URL is percent-encoded and this is not, because the server
    hashes what it decoded; swapping them breaks exactly the accounts with a space or a plus in the
    name and no others.
*/
static void signNumber(md5_state_t* md5, u32 value) {
	char number[11];
	int  n = 0;
	int  i;

	if (value == 0) {
		number[n++] = '0';
	}
	while (value && n < (int)sizeof(number)) {
		number[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	for (i = 0; i < n; i++) {
		const char c = number[n - 1 - i];

		md5_append(md5, (const md5_byte_t*)&c, 1);
	}
}

void raQueueSign(u32 id, const char* username, int hardcore, u32 seconds, char* out) {
	static const char hex[] = "0123456789abcdef";
	md5_state_t       md5;
	md5_byte_t        digest[16];
	int               i;

	md5_init(&md5);

	signNumber(&md5, id);

	md5_append(&md5, (const md5_byte_t*)username, (int)strlen(username));

	{
		const char flag = hardcore ? '1' : '0';

		md5_append(&md5, (const md5_byte_t*)&flag, 1);
	}

	if (seconds) {
		signNumber(&md5, id);
		signNumber(&md5, seconds);
	}

	md5_finish(&md5, digest);

	for (i = 0; i < 16; i++) {
		out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
		out[i * 2 + 1] = hex[digest[i] & 0xF];
	}
	out[32] = 0;
}

#endif /* RA_LAUNCHER_WIFI */
