/*
    Step 3d: turn an `r=patch` reply into the staging block, without ever holding the reply.

    This is the last rung of the network ladder and the first one that produces something a
    game will run. Everything before it was a measurement -- the chip came up, the API answered,
    the server knew the ROM. This one takes the server's own achievement definitions and puts
    them exactly where docs/retroachievements.md said they would go from the beginning:
    launcher -> staging -> DSi WRAM, the same path a hand-written ra_achievements.txt already
    travels through loadRaDefinitions().

    Why it is a scanner and not a parser
    ------------------------------------
    The reply for this game is over 100 K. The launcher has about 85 K of heap left once lwip is
    up, and the block the definitions are going to is 32 K. So there is no buffer that could
    hold the document, and a JSON parser needs the document. The bytes are therefore read
    through: whatever recv() hands over is scanned, the definitions are copied out, and the rest
    is dropped on the floor as it goes past.

    What makes that sound rather than lucky is a property of JSON rather than of RA: a quote
    inside a string is escaped as `\"`, so the eleven bytes `"MemAddr":"` cannot appear inside a
    value. An achievement whose title is literally `"MemAddr":"` arrives as `\"MemAddr\":\"` and
    does not match, because byte nine is a backslash where the needle wants a quote. The needle
    can only ever be a key.

    Two limitations, stated rather than discovered later
    ---------------------------------------------------
    It does not know which object the key belonged to. Today `MemAddr` is an achievement field
    and nothing else -- leaderboards carry `Mem`, rich presence `RichPresencePatch` -- so this
    reads the achievement set and only that. A future `MemAddr` somewhere else in the reply
    would be read too.

    And `Flags` arrives *after* `MemAddr` in each object, which is why a definition is held here
    before it is committed. RA sends unofficial achievements in the same array as published
    ones, distinguished only by that field: 3 is core, 5 is unofficial. Filtering them is the
    difference between "the set fits the block" and a number inflated by achievements no player
    is scored on -- so a definition waits until its flag arrives, or until the next `MemAddr`
    or the end of the stream says none is coming.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <string.h>

/*
    The two needles. Both are matched at once while scanning, because either can come next: a
    `Flags` closes the definition being held and a `MemAddr` opens the following one.
*/
static const char raPatchSayMemAddr[] = "\"MemAddr\":\"";
static const char raPatchSayFlags[]   = "\"Flags\":";

/* RA's own two values for the field. 3 is published; 5 is unofficial and not scored. */
#define RA_PATCH_FLAGS_CORE       3
#define RA_PATCH_FLAGS_UNOFFICIAL 5

void raPatchReset(raPatch* p, char* block, u32 blockMax) {
	memset(p, 0, sizeof(*p));
	p->block    = block;
	p->blockMax = blockMax;
	p->state    = RA_PATCH_SCAN;
	if (block && blockMax) {
		block[0] = 0;
	}
}

/*
    Advance a needle by one byte.

    Restart-and-retest rather than a full KMP failure function, and that is correct here rather
    than approximately correct: it is exact whenever no proper prefix of length two or more is
    also a suffix of a longer prefix. For `"MemAddr":"` the only repeated character is the
    quote, at length one, which the retest covers -- so `""MemAddr":"` matches, and
    tools/ra_launcher_test.c feeds it exactly that. `"Flags":` has no borders at all.

    Written as a helper so the property is stated in one place. A third needle would have to be
    checked against it.
*/
static u8 raPatchAdvance(const char* needle, u8 at, char c) {
	if (needle[at] == c) {
		return (u8)(at + 1);
	}
	return (u8)(needle[0] == c ? 1 : 0);
}

/*
    Put the held definition in the block, or account for why it did not go.

    Every outcome is counted, and that is the point of the function: a set where thirty
    definitions were silently too long looks, from the block, exactly like a set with thirty
    fewer achievements. `wanted` accumulates what the definitions that *should* have been
    written needed, whether they fit or not, so a full block reports how full it would have
    had to be.
*/
static void raPatchCommit(raPatch* p) {
	u32 length;

	if (!p->pendingOpen) {
		return;
	}

	length = p->pendingLength;

	if (p->pendingSeen > p->longest) {
		p->longest = p->pendingSeen;
	}

	if (p->pendingBad) {
		/*
		    Dropped rather than truncated, deliberately. A truncated memaddr is not a shorter
		    achievement, it is a different one -- rcheevos would either refuse it or, worse,
		    parse the surviving prefix into a condition that triggers when it should not. So the
		    length is reported and the definition is not kept.
		*/
		p->tooLong++;
	} else if (p->pendingSeen == 0) {
		p->empty++;
	} else if (p->flagsSeen && p->flags == RA_PATCH_FLAGS_UNOFFICIAL) {
		p->unofficial++;
	} else {
		if (!p->flagsSeen) {
			p->noFlags++;
		} else if (p->flags != RA_PATCH_FLAGS_CORE) {
			/*
			    Kept, and counted so it is visible. An unknown flag is more likely to be a
			    value RA added than a definition to throw away, and the log naming it is how
			    that gets noticed at all.
			*/
			p->oddFlags++;
		}

		p->wanted += length + 1;   /* the newline the reader splits on */

		/*
		    Zero is the unset marker rather than a counter, and it is safe as one: an empty value
		    is filtered above, so no definition that reaches here has length 0.
		*/
		if (p->shortest == 0 || length < p->shortest) {
			p->shortest = length;
		}

		if (p->block && p->used + length + 1 <= p->blockMax) {
			memcpy(p->block + p->used, p->pending, length);
			p->used += length;
			p->block[p->used++] = '\n';
			p->block[p->used]   = 0;
			p->kept++;
		} else {
			p->dropped++;
		}
	}

	p->pendingOpen   = 0;
	p->pendingBad    = 0;
	p->pendingSeen   = 0;
	p->pendingLength = 0;
	p->flagsSeen     = 0;
	p->flags         = 0;
}

/* One decoded byte of the value being read. */
static void raPatchPending(raPatch* p, char c) {
	p->pendingSeen++;
	if (p->pendingLength < RA_PATCH_MEMADDR_MAX - 1) {
		p->pending[p->pendingLength++] = c;
	} else {
		p->pendingBad = 1;
	}
}

/*
    Feed the scanner. Signature is raNetSink's, so it can be handed straight to
    raNetHttpGetStream() with no adapter -- which is also why the first parameter is a void*.
*/
void raPatchFeed(void* ctx, const char* data, int length) {
	raPatch* p = (raPatch*)ctx;
	int      i;

	for (i = 0; i < length; i++) {
		const char c = data[i];

		if (p->state == RA_PATCH_VALUE) {
			if (p->escape) {
				p->escape = 0;
				/*
				    RA escapes forward slashes, and memaddr syntax uses `/` as its division
				    operator -- so `\/` arriving as two characters would make every divide a
				    parse error. The other two are here because JSON allows them and because
				    handling `\"` is what lets the closing quote below be trusted.

				    Anything else keeps its backslash. A `\u` escape cannot appear in a memaddr
				    and inventing a decoder for one would be inventing a bug.
				*/
				if (c == '/' || c == '\\' || c == '"') {
					raPatchPending(p, c);
				} else {
					raPatchPending(p, '\\');
					raPatchPending(p, c);
				}
				continue;
			}
			if (c == '\\') {
				p->escape = 1;
				continue;
			}
			if (c == '"') {
				/*
				    End of the value -- but not the end of the definition. It stays held until
				    its Flags field arrives, because that is what decides whether it belongs to
				    the published set. See the note at the top of the file.
				*/
				p->state   = RA_PATCH_SCAN;
				p->memAt   = 0;
				p->flagsAt = 0;
				continue;
			}
			raPatchPending(p, c);
			continue;
		}

		if (p->state == RA_PATCH_FLAGS) {
			if (c >= '0' && c <= '9') {
				if (p->flags < 100000) {
					p->flags = p->flags * 10 + (u32)(c - '0');
				}
				p->flagsSeen = 1;
				continue;
			}
			/*
			    Whatever ended the digits is not consumed here -- it falls through to the
			    scanner below in this same iteration. In RA's reply that character is a comma,
			    but it could as easily be the quote that opens the next key, and a scanner that
			    skipped one byte after every Flags field would be a scanner that works until it
			    does not.
			*/
			raPatchCommit(p);
			p->state   = RA_PATCH_SCAN;
			p->memAt   = 0;
			p->flagsAt = 0;
		}

		p->memAt   = raPatchAdvance(raPatchSayMemAddr, p->memAt, c);
		p->flagsAt = raPatchAdvance(raPatchSayFlags, p->flagsAt, c);

		if (raPatchSayMemAddr[p->memAt] == 0) {
			/*
			    A definition still held here means its object ended without a Flags field, or
			    with one this scanner never saw. Committed rather than dropped: a definition
			    the server sent is worth more than a flag we did not find.
			*/
			raPatchCommit(p);
			p->state       = RA_PATCH_VALUE;
			p->pendingOpen = 1;
			p->escape      = 0;
			p->memAt       = 0;
			p->flagsAt     = 0;
			continue;
		}
		if (raPatchSayFlags[p->flagsAt] == 0) {
			p->state     = RA_PATCH_FLAGS;
			p->flags     = 0;
			p->flagsSeen = 0;
			p->memAt     = 0;
			p->flagsAt   = 0;
		}
	}
}

/*
    End of the stream. Commits whatever was held and leaves the block terminated.

    Has to exist rather than being folded into the feed, because the last achievement in the
    reply is the one whose Flags field is followed by the end of the document instead of by
    another key -- so without this the set is always one definition short. That is the kind of
    off-by-one that a hundred-achievement set hides perfectly well.
*/
void raPatchFinish(raPatch* p) {
	if (p->state == RA_PATCH_FLAGS) {
		raPatchCommit(p);
	} else if (p->state == RA_PATCH_VALUE) {
		/*
		    The stream ended mid-value: the connection dropped, or the reply was cut short. The
		    bytes in hand are a *prefix* of a definition, and a prefix is a different definition
		    -- so it goes nowhere. Counted on its own rather than as a too-long one, because the
		    two say opposite things about what to do next: one is a buffer to enlarge, the other
		    is a request to repeat.
		*/
		p->cutShort++;
		p->pendingOpen   = 0;
		p->pendingSeen   = 0;
		p->pendingLength = 0;
	} else {
		raPatchCommit(p);
	}
	p->state = RA_PATCH_SCAN;

	if (p->block && p->blockMax) {
		p->block[p->used] = 0;
	}
}

#endif /* RA_LAUNCHER_WIFI */
