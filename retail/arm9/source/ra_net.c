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

#endif /* RA_LAUNCHER_WIFI */
