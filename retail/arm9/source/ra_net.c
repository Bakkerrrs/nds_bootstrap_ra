/*
    ra_net: the HTTP side of talking to RetroAchievements, from the launcher.

    The layering table in docs/retroachievements.md has had a row for this since before any of
    it existed -- "HTTP(S) transport to the RA servers; rcheevos ships no networking" -- and
    step 3c is where it stops being a plan. The GET lived in ra_wifi.c while there was exactly
    one request to make; with `r=login` there are two, and `r=patch` makes three.

    Plain HTTP, deliberately and with the cost stated once: `dorequest.php` answers on port 80
    with the same JSON as HTTPS, which is what makes any of this possible on a console with no
    TLS. It also means credentials cross the network in the clear. See *#1a*.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <stdio.h>
#include <string.h>

/*
    The socket half is compiled out for the host test, which reaches in for raNetUrlEncode() and
    raNetJsonString() -- both pure string logic -- without dragging lwip onto a PC. Defined
    nowhere in the target build, so the console always gets the whole file.
*/
#ifndef RA_NET_HOST_TEST
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#endif

/*
    Percent-encode into a query string.

    This is the part of 3c most likely to fail silently, which is why it exists as a function
    with a host test rather than as a sprintf. A password is user text: an `&` ends the
    parameter early, a `+` decodes as a space, a `%` starts an escape that is not there, and
    every one of those produces a perfectly well-formed request that comes back
    `invalid_credentials`. Indistinguishable, from the console, from a wrong password -- and
    the user would be told their password is wrong when it is not.

    Unreserved characters pass through per RFC 3986; everything else goes as %XX with uppercase
    hex. Truncates rather than overruns, and says which by returning false, because a silently
    shortened password is the same bug in a different coat.
*/
bool raNetUrlEncode(const char* in, char* out, size_t outSize) {
	static const char hex[] = "0123456789ABCDEF";
	size_t            o     = 0;
	size_t            i;

	if (outSize == 0) {
		return false;
	}
	for (i = 0; in[i]; i++) {
		const unsigned char c = (unsigned char)in[i];

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
		 || c == '-' || c == '_' || c == '.' || c == '~') {
			if (o + 1 >= outSize) {
				out[o] = 0;
				return false;
			}
			out[o++] = (char)c;
		} else {
			if (o + 3 >= outSize) {
				out[o] = 0;
				return false;
			}
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xF];
		}
	}
	out[o] = 0;
	return true;
}

/*
    One GET, and the reply read into a caller-supplied buffer.

    The recv() loop is bounded by SO_RCVTIMEO, which is not a refinement: without it this hung
    the console twice on the line after the request went out. The reply arrives in one packet
    and the server's FIN is a separate event that may never turn up, so a blocking read with no
    deadline waits for it forever. A timeout with bytes already in hand is success -- what the
    caller checks is the body.

    Returns bytes received, or a negative RA_NET_* code so a failure names its own step.
*/
#ifndef RA_NET_HOST_TEST
int raNetHttpGet(const char* host, const char* path, char* out, int outSize, raNetProgress* p) {
	struct hostent*    he;
	struct sockaddr_in addr;
	char               request[512];
	int                sock;
	int                total = 0;

	if (outSize < 2) {
		return RA_NET_BAD_ARGS;
	}
	out[0] = 0;

	he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) {
		return RA_NET_NO_DNS;
	}
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	if (p) {
		p->resolved = 1;
		p->address  = addr.sin_addr.s_addr;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		return RA_NET_NO_SOCKET;
	}
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(RA_NET_PORT);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		lwip_close(sock);
		return RA_NET_NO_CONNECT;
	}
	if (p) {
		p->connected = 1;
	}

	/* A user agent is mandatory on every Connect API call; without one it is our fault. */
	if (sniprintf(request, sizeof(request),
	              "GET %s HTTP/1.1\r\n"
	              "Host: %s\r\n"
	              "User-Agent: nds-bootstrap-ra/0.1\r\n"
	              "Connection: close\r\n"
	              "\r\n",
	              path, host) >= (int)sizeof(request)) {
		lwip_close(sock);
		return RA_NET_REQ_TOO_LONG;
	}

	if (send(sock, request, strlen(request), 0) < 0) {
		lwip_close(sock);
		return RA_NET_NO_SEND;
	}
	if (p) {
		p->sent = 1;
	}

	{
		struct timeval tv;

		tv.tv_sec  = RA_NET_RECV_TIMEOUT;
		tv.tv_usec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	while (total < outSize - 1) {
		const int got = recv(sock, out + total, outSize - 1 - total, 0);

		if (got <= 0) {
			if (p) {
				p->closedByPeer = (got == 0);
			}
			break;
		}
		total += got;
	}
	out[total] = 0;
	lwip_close(sock);
	return total;
}
#endif /* RA_NET_HOST_TEST */

/*
    ------------------------------------------------------------------------------------
    Step 3d: the same GET, but never held.

    `r=patch` returns the whole achievement set. For a game like this one that is well over
    100 K -- more than the ~85 K of heap the launcher has left once lwip is up, and more than
    three times the 32 K staging block the definitions are going to. There is no buffer to read
    it into, so it is read *through*: recv() into a small window, hand each window to a sink,
    forget it.

    Two things have to be undone on the way, and only one of them is obvious.

    The headers are the obvious one, and the boundary can fall anywhere -- including between
    the two CRLFs -- so it is a state machine rather than a strstr.

    The other is chunked transfer encoding, and it is the reason this exists as testable code
    instead of a loop inside raWifiProbe(). An HTTP/1.1 reply may arrive with its body cut into
    chunks, each prefixed by a hex length *written into the byte stream*. Every reply this
    project has read so far came back unchunked -- the bodies in the logs start at `{` -- but
    those were one packet each, and a CDN's decision to chunk is a decision about size. A
    `1a2f\r\n` landing inside a memaddr string would produce one corrupt definition out of a
    hundred and nothing anywhere would say so. That is precisely the class of failure this
    project keeps paying for, so the framing is undone here, where a host test can split the
    input at every byte boundary and check it.
    ------------------------------------------------------------------------------------
*/

void raNetStreamReset(raNetStream* s, raNetSink sink, void* ctx) {
	memset(s, 0, sizeof(*s));
	s->sink  = sink;
	s->ctx   = ctx;
	s->state = RA_NET_STREAM_STATUS;
}

static void raNetStreamEmit(raNetStream* s, const char* data, int length) {
	if (length <= 0) {
		return;
	}
	s->bodyBytes += (u32)length;
	if (s->sink) {
		s->sink(s->ctx, data, length);
	}
}

/*
    One finished header line. Only two of them are read, and the rest are deliberately ignored:
    Content-Length is not needed because `Connection: close` and the chunk terminator both say
    where the body ends, and a length we believed but did not enforce would be worse than none.
*/
static void raNetStreamHeaderLine(raNetStream* s) {
	s->line[s->lineLength] = 0;

	if (s->state == RA_NET_STREAM_STATUS) {
		/* "HTTP/1.1 200 OK" -- the code, or 0 if this is not an HTTP reply at all. */
		const char* at = strchr(s->line, ' ');

		if (at) {
			u32 value = 0;
			int digits = 0;

			while (*++at == ' ') {
			}
			while (*at >= '0' && *at <= '9' && digits < 3) {
				value = value * 10 + (u32)(*at - '0');
				digits++;
				at++;
			}
			if (digits == 3) {
				s->status = (u16)value;
			}
		}
		s->state      = RA_NET_STREAM_HEADER;
		s->lineLength = 0;
		return;
	}

	if (s->lineLength == 0) {
		/* The blank line. Body from here, framed or not. */
		s->state = s->chunked ? RA_NET_STREAM_SIZE : RA_NET_STREAM_BODY;
		return;
	}

	/*
	    Case-insensitive, because header names are and a server is under no obligation to send
	    the capitalisation anyone expects. Matched on the value containing "chunked" rather than
	    equalling it: the field is a list, and `gzip, chunked` is legal even if nothing here
	    would survive the gzip.
	*/
	{
		static const char name[] = "transfer-encoding";
		u32               i;

		for (i = 0; name[i]; i++) {
			char c = s->line[i];

			if (c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			if (c != name[i]) {
				break;
			}
		}
		if (!name[i]) {
			u32 j;

			for (j = i; s->line[j]; j++) {
				if (s->line[j] >= 'A' && s->line[j] <= 'Z') {
					s->line[j] = (char)(s->line[j] - 'A' + 'a');
				}
			}
			if (strstr(s->line + i, "chunked")) {
				s->chunked = 1;
			}
		}
	}
	s->lineLength = 0;
}

void raNetStreamFeed(raNetStream* s, const char* data, int length) {
	int i = 0;

	while (i < length) {
		switch (s->state) {
		case RA_NET_STREAM_STATUS:
		case RA_NET_STREAM_HEADER:
			if (data[i] == '\n') {
				raNetStreamHeaderLine(s);
			} else if (data[i] != '\r') {
				/*
				    An over-long header is truncated rather than allowed to overrun, and
				    truncation is safe *here* in a way it never is for a definition: the two
				    things read out of a header are both at its start.
				*/
				if (s->lineLength < sizeof(s->line) - 1) {
					s->line[s->lineLength++] = data[i];
				}
			}
			i++;
			break;

		case RA_NET_STREAM_BODY:
			/* Identity encoding: the rest of this window is body, in one piece. */
			raNetStreamEmit(s, data + i, length - i);
			i = length;
			break;

		case RA_NET_STREAM_SIZE:
			if (data[i] == '\n') {
				s->line[s->lineLength] = 0;
				s->lineLength          = 0;
				s->chunkLeft           = 0;
				{
					const char* at = s->line;

					while (*at) {
						u32 digit;

						if (*at >= '0' && *at <= '9') {
							digit = (u32)(*at - '0');
						} else if (*at >= 'a' && *at <= 'f') {
							digit = (u32)(*at - 'a' + 10);
						} else if (*at >= 'A' && *at <= 'F') {
							digit = (u32)(*at - 'A' + 10);
						} else {
							/* `;` starts a chunk extension, and anything else ends the number. */
							break;
						}
						s->chunkLeft = s->chunkLeft * 16 + digit;
						at++;
					}
				}
				/* A zero-length chunk is the end of the body. Trailers are not ours to want. */
				s->state = s->chunkLeft ? RA_NET_STREAM_DATA : RA_NET_STREAM_TRAILER;
			} else if (data[i] != '\r' && s->lineLength < sizeof(s->line) - 1) {
				s->line[s->lineLength++] = data[i];
			}
			i++;
			break;

		case RA_NET_STREAM_DATA:
			{
				u32 n = (u32)(length - i);

				if (n > s->chunkLeft) {
					n = s->chunkLeft;
				}
				raNetStreamEmit(s, data + i, (int)n);
				s->chunkLeft -= n;
				i            += (int)n;
				if (s->chunkLeft == 0) {
					s->state = RA_NET_STREAM_CRLF;
				}
			}
			break;

		case RA_NET_STREAM_CRLF:
			/* Consume the CRLF after a chunk, one byte at a time -- it can be split too. */
			if (data[i] == '\n') {
				s->state = RA_NET_STREAM_SIZE;
			}
			i++;
			break;

		default:
			/* RA_NET_STREAM_TRAILER: past the last chunk. Read and discarded. */
			i = length;
			break;
		}
	}
}

/*
    The socket half of the same thing. Same DNS, same connect, same per-recv() deadline as
    raNetHttpGet() -- the difference is only that nothing accumulates.

    Returns the number of *body* bytes that reached the sink, or a negative RA_NET_* code.
    Body rather than wire bytes on purpose: it is the number that can be compared against what
    the scanner made of them.
*/
#ifndef RA_NET_HOST_TEST
int raNetHttpGetStream(const char* host, const char* path, raNetSink sink, void* ctx,
                       raNetProgress* p) {
	static raNetStream stream;
	static char        rx[1024];
	struct hostent*    he;
	struct sockaddr_in addr;
	char               request[512];
	int                sock;

	he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) {
		return RA_NET_NO_DNS;
	}
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	if (p) {
		p->resolved = 1;
		p->address  = addr.sin_addr.s_addr;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		return RA_NET_NO_SOCKET;
	}
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(RA_NET_PORT);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		lwip_close(sock);
		return RA_NET_NO_CONNECT;
	}
	if (p) {
		p->connected = 1;
	}

	if (sniprintf(request, sizeof(request),
	              "GET %s HTTP/1.1\r\n"
	              "Host: %s\r\n"
	              "User-Agent: nds-bootstrap-ra/0.1\r\n"
	              "Connection: close\r\n"
	              "\r\n",
	              path, host) >= (int)sizeof(request)) {
		lwip_close(sock);
		return RA_NET_REQ_TOO_LONG;
	}
	if (send(sock, request, strlen(request), 0) < 0) {
		lwip_close(sock);
		return RA_NET_NO_SEND;
	}
	if (p) {
		p->sent = 1;
	}

	{
		struct timeval tv;

		tv.tv_sec  = RA_NET_RECV_TIMEOUT;
		tv.tv_usec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	raNetStreamReset(&stream, sink, ctx);
	for (;;) {
		const int got = recv(sock, rx, sizeof(rx), 0);

		if (got <= 0) {
			if (p) {
				p->closedByPeer = (got == 0);
			}
			break;
		}
		raNetStreamFeed(&stream, rx, got);
	}
	lwip_close(sock);

	/*
	    A status the caller can act on, without a second out-parameter: the sink already has the
	    definitions, and anything other than 200 means it has none for a reason worth naming.
	*/
	if (stream.status && stream.status != 200) {
		return -(int)(1000 + stream.status);
	}
	return (int)stream.bodyBytes;
}
#endif /* RA_NET_HOST_TEST */

/* The body of an HTTP reply, or the whole thing if the headers cannot be found. */
const char* raNetBody(const char* response) {
	const char* body = strstr(response, "\r\n\r\n");

	return body ? body + 4 : response;
}

/*
    Pull one string field out of a JSON object, without a JSON parser.

    Enough for `r=login`, whose reply is a flat object of scalars, and deliberately not enough
    for `r=patch`, which is nested and is step 3d's problem. Written this way so 3c does not
    drag a parser in before there is something that needs one -- and so the thing it does do is
    small enough to be obviously right.

    Matches `"key":"` and copies to the closing quote. Does not decode escapes: a token is
    base64-ish and a username cannot contain a quote, so the first `"` really is the end.
*/
bool raNetJsonString(const char* json, const char* key, char* out, size_t outSize) {
	char        needle[40];
	const char* at;
	size_t      i = 0;

	if (outSize == 0) {
		return false;
	}
	out[0] = 0;

	if (sniprintf(needle, sizeof(needle), "\"%s\":\"", key) >= (int)sizeof(needle)) {
		return false;
	}
	at = strstr(json, needle);
	if (!at) {
		return false;
	}
	at += strlen(needle);

	while (at[i] && at[i] != '"') {
		if (i + 1 >= outSize) {
			out[i] = 0;
			return false;
		}
		out[i] = at[i];
		i++;
	}
	out[i] = 0;
	return i > 0;
}

/*
    Pull one *numeric* field out of a JSON object.

    Separate from raNetJsonString() rather than folded into it, because the distinction is the
    whole point for `r=gameid`: the reply is `{"Success":true,"GameID":1448}` and the field is a
    bare number. A matcher that accepted a quote here would happily read the first digits of a
    string field and return a plausible game that is not the game.

    So the needle is `"key":` and the value must begin with a digit. `"GameID":"1448"` fails,
    which is correct: that is not what the API sends, and if it ever did, silence beats a guess.

    Zero is a legitimate value and is returned as one -- see the caller. For `r=gameid` it means
    the server does not recognise the hash, which is an answer rather than a failure.
*/
bool raNetJsonNumber(const char* json, const char* key, u32* out) {
	char        needle[40];
	const char* at;
	u32         value = 0;
	int         digits = 0;

	*out = 0;
	if (sniprintf(needle, sizeof(needle), "\"%s\":", key) >= (int)sizeof(needle)) {
		return false;
	}
	at = strstr(json, needle);
	if (!at) {
		return false;
	}
	at += strlen(needle);

	/* Whitespace is legal JSON between the colon and the value, even if this API sends none. */
	while (*at == ' ' || *at == '\t') {
		at++;
	}
	while (*at >= '0' && *at <= '9') {
		/* Refuse rather than wrap: a truncated game ID is a different game. */
		if (value > (0xFFFFFFFFu - (u32)(*at - '0')) / 10) {
			return false;
		}
		value = value * 10 + (u32)(*at - '0');
		digits++;
		at++;
	}
	if (digits == 0) {
		return false;
	}
	*out = value;
	return true;
}

#endif /* RA_LAUNCHER_WIFI */
