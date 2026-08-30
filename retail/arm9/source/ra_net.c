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
#include <nds.h>   /* swiWaitForVBlank(), for the gap between connect attempts */
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
    Read `Content-Length` out of a raw response's headers, or 0 if it does not say one.

    Its own function, and pure, so tools/ra_launcher_test.c can drive it against the shapes a real
    server sends: the header is case-insensitive by the standard and Cloudflare does not send it the
    way anyone expects, the value may be preceded by spaces, and a `Content-Length` appearing inside
    the *body* must not be found. Bounded by `headerEnd` for that last reason.
*/
u32 raNetHeaderLength(const char* response, u32 headerEnd) {
	static const char key[] = "content-length:";
	u32               at;

	for (at = 0; at < headerEnd; at++) {
		u32 i;

		if (at && response[at - 1] != '\n') {
			continue;
		}
		for (i = 0; key[i] && at + i < headerEnd; i++) {
			char c = response[at + i];

			if (c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			if (c != key[i]) {
				break;
			}
		}
		if (!key[i]) {
			u32 value = 0;
			u32 j     = at + i;

			while (j < headerEnd && response[j] == ' ') {
				j++;
			}
			while (j < headerEnd && response[j] >= '0' && response[j] <= '9') {
				value = value * 10 + (u32)(response[j] - '0');
				j++;
			}
			return value;
		}
	}
	return 0;
}

/* ...and whether it said `Transfer-Encoding: chunked`, on the same terms. */
int raNetHeaderChunked(const char* response, u32 headerEnd) {
	static const char key[] = "transfer-encoding:";
	u32               at;

	for (at = 0; at < headerEnd; at++) {
		u32 i;

		if (at && response[at - 1] != '\n') {
			continue;
		}
		for (i = 0; key[i] && at + i < headerEnd; i++) {
			char c = response[at + i];

			if (c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			if (c != key[i]) {
				break;
			}
		}
		if (!key[i]) {
			u32 j;

			for (j = at + i; j < headerEnd && response[j] != '\n'; j++) {
				char c = response[j];

				if (c >= 'A' && c <= 'Z') {
					c = (char)(c - 'A' + 'a');
				}
				if (c == 'c' && j + 7 <= headerEnd
				    && strncmp(response + j, "chunked", 7) == 0) {
					return 1;
				}
			}
			return 0;
		}
	}
	return 0;
}

/*
    DNS, then a socket, then connect -- with a bounded retry, because the third socket of a run once
    lost a race inside lwip. See RA_NET_CONNECT_TRIES.

    Factored out of the two GETs rather than duplicated into both, so a retry policy exists in one
    place. Returns a connected socket, or a negative RA_NET_* code.
*/
#ifndef RA_NET_HOST_TEST
static int raNetConnect(const char* host, raNetProgress* p) {
	struct hostent*    he;
	struct sockaddr_in addr;
	int                tries;

	/*
	    Resolved once per boot, not once per request.

	    Five rungs each open their own connection -- every request carries `Connection: close`,
	    because a DS with 191K of heap after lwip is up is not a place to keep sockets alive across
	    stages -- and each of those was asking DNS again. lwip caches, but a cache miss is a round
	    trip to the resolver on a link this fork has measured at its slowest, and there is no version
	    of "the address of retroachievements.org" that changes between two rungs of the same boot.
	*/
	static u32 cachedAddr;

	if (cachedAddr) {
		memset(&addr, 0, sizeof(addr));
		addr.sin_addr.s_addr = cachedAddr;
	} else {
		he = gethostbyname(host);
		if (!he || !he->h_addr_list[0]) {
			return RA_NET_NO_DNS;
		}
		memset(&addr, 0, sizeof(addr));
		memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
		cachedAddr = addr.sin_addr.s_addr;
	}
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(RA_NET_PORT);

	if (p) {
		p->resolved = 1;
		p->address  = addr.sin_addr.s_addr;
	}

	for (tries = 1; tries <= RA_NET_CONNECT_TRIES; tries++) {
		const int sock = socket(AF_INET, SOCK_STREAM, 0);

		if (p) {
			p->attempts = (u8)tries;
		}
		if (sock < 0) {
			/* No socket at all is a different failure from a refused connect, and says so. */
			if (tries == RA_NET_CONNECT_TRIES) {
				return RA_NET_NO_SOCKET;
			}
		} else if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
			if (p) {
				p->connected = 1;
			}
			return sock;
		} else {
			lwip_close(sock);
		}

		/*
		    A gap before trying again, and it is doing work rather than marking time: lwip's own
		    processing runs off a 100 ms TIMER3 in dsiwifi's ARM9 half, so waiting frames is what
		    lets a half-finished netconn finish and go back to the pool.
		*/
		if (tries < RA_NET_CONNECT_TRIES) {
			int frames = RA_NET_RETRY_FRAMES;

			while (frames-- > 0) {
				swiWaitForVBlank();
			}
		}
	}
	return RA_NET_NO_CONNECT;
}
#endif /* RA_NET_HOST_TEST */

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
	char request[512];
	int  sock;
	int  total = 0;

	if (outSize < 2) {
		return RA_NET_BAD_ARGS;
	}
	out[0] = 0;

	sock = raNetConnect(host, p);
	if (sock < 0) {
		return sock;
	}

	/* A user agent is mandatory on every Connect API call; without one it is our fault. */
	if (sniprintf(request, sizeof(request),
	              "GET %s HTTP/1.1\r\n"
	              "Host: %s\r\n"
	              "User-Agent: " RA_NET_USER_AGENT "\r\n"
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

	{
		/*
		    The same early exit, and the four rungs that use this path are where it was measured:
		    r=login, r=gameid, r=startsession and r=unlocks each took 10.2 seconds for about a
		    kilobyte, against 5 seconds for the whole chip bring-up and DHCP lease.

		    This reader keeps the raw reply -- status line, headers and body -- because that is what
		    its callers were written against, so the header scan happens here rather than in the
		    stream parser. Content-Length is read once, when the blank line arrives, and the read
		    ends when that many body bytes have followed it. A chunked reply ends on its terminator
		    instead, and a reply with neither still ends where it always did, at the close.
		*/
		u32  headerEnd = 0;
		u32  bodyWanted = 0;
		int  chunked = 0;

		while (total < outSize - 1) {
			const int got = recv(sock, out + total, outSize - 1 - total, 0);

			if (got <= 0) {
				if (p) {
					p->closedByPeer = (got == 0);
				}
				break;
			}
			total += got;
			out[total] = 0;

			if (!headerEnd) {
				const char* blank = strstr(out, "\r\n\r\n");

				if (blank) {
					headerEnd  = (u32)(blank + 4 - out);
					bodyWanted = raNetHeaderLength(out, headerEnd);
					chunked    = raNetHeaderChunked(out, headerEnd);
				}
			}
			if (headerEnd) {
				if (bodyWanted && (u32)total - headerEnd >= bodyWanted) {
					break;
				}
				if (chunked && total >= 5
				    && memcmp(out + total - 5, "0\r\n\r\n", 5) == 0) {
					break;
				}
			}
		}
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
    One finished header line. Three of them are read now, and the third is the one that matters:
    Content-Length says where the body ends *without waiting for the socket to*.

    The note here used to say Content-Length was not needed, because `Connection: close` and the
    chunk terminator both give the same answer. Both do. What that missed is the cost: waiting for a
    close means waiting, and the timing line put every one-kilobyte request at 10.2 seconds against a
    chip and a DHCP lease that together cost 5. Believed but not enforced would indeed be worse than
    none -- so it is enforced, and the counted bytes are what end the read.
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

	/* ...and the length, when the reply gives one. */
	{
		static const char length[] = "content-length:";
		u32               i;

		for (i = 0; length[i]; i++) {
			char c = s->line[i];

			if (c >= 'A' && c <= 'Z') {
				c = (char)(c - 'A' + 'a');
			}
			if (c != length[i]) {
				break;
			}
		}
		if (!length[i]) {
			u32 value = 0;

			while (s->line[i] == ' ') {
				i++;
			}
			while (s->line[i] >= '0' && s->line[i] <= '9') {
				value = value * 10 + (u32)(s->line[i] - '0');
				i++;
			}
			s->contentLength = value;
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
				if (!s->chunkLeft) {
					s->done = 1;
				}
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
	char               request[512];
	int                sock;

	sock = raNetConnect(host, p);
	if (sock < 0) {
		return sock;
	}

	if (sniprintf(request, sizeof(request),
	              "GET %s HTTP/1.1\r\n"
	              "Host: %s\r\n"
	              "User-Agent: " RA_NET_USER_AGENT "\r\n"
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
		/*
		    Stop the moment the body is whole, instead of reading on until the peer closes. See
		    raNetStream.contentLength -- this is where the wait was.
		*/
		if (stream.done) {
			break;
		}
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

/*
    Pull one *boolean* field out of a JSON object.

    A third reader rather than a case in the other two, because the failure it has to avoid is
    specific: `r=awardachievement` answers `{"Success":true,...}` and `{"Success":false,"Error":...}`,
    and those two replies differ by five characters in a body that is otherwise identical. Reaching
    for strstr(body, "true") anywhere in the reply would find the word inside an achievement title
    just as happily.

    So it matches `"key":` and then requires the value to be exactly the literal `true`. Anything
    else -- `false`, a number, a string, a missing key -- is false, and false is the safe direction:
    it means an id stays reported as refused with the body logged, rather than an unlock being
    counted as awarded because the word appeared somewhere.
*/
bool raNetJsonTrue(const char* json, const char* key) {
	char        needle[40];
	const char* at;

	if (sniprintf(needle, sizeof(needle), "\"%s\":", key) >= (int)sizeof(needle)) {
		return false;
	}
	at = strstr(json, needle);
	if (!at) {
		return false;
	}
	at += strlen(needle);

	while (*at == ' ' || *at == '\t') {
		at++;
	}
	return strncmp(at, "true", 4) == 0;
}

/*
    A JSON array of bare integers, into a caller-supplied array.

    `r=unlocks` answers `{"Success":true,"UserUnlocks":[93119,93120],"GameID":14856}`, and this is the
    one shape the flat matchers above could not read. Still not a JSON parser: it finds `"key":[`,
    then takes runs of digits until the closing bracket, and anything that is not a digit or a comma
    or whitespace ends it.

    **-1 for a missing key, 0 for an empty list**, and conflating those would be the bug worth
    avoiding: a player who has earned nothing and a request that failed produce the same empty array,
    but the first means "stage everything because nothing is done" and the second means "stage
    everything because we do not know". Both stage everything -- and only the second is worth
    reporting as a problem.

    Overflow is truncation and it is reported by returning `max`: the caller compares against its own
    capacity. Refusing outright would be worse here, since a partial skip list is still correct --
    it only means a few already-earned achievements get staged again.
*/
int raNetJsonIdList(const char* json, const char* key, u32* out, int max) {
	char        needle[40];
	const char* at;
	int         count = 0;

	if (sniprintf(needle, sizeof(needle), "\"%s\":[", key) >= (int)sizeof(needle)) {
		return -1;
	}
	at = strstr(json, needle);
	if (!at) {
		return -1;
	}
	at += strlen(needle);

	while (*at && *at != ']') {
		if (*at >= '0' && *at <= '9') {
			u32 value  = 0;
			int digits = 0;

			while (*at >= '0' && *at <= '9') {
				const u32 digit = (u32)(*at - '0');

				/* Refused rather than wrapped, like every other number this project reads. */
				if (value > (0xFFFFFFFFu - digit) / 10u) {
					digits = 0;
					break;
				}
				value = value * 10 + digit;
				digits++;
				at++;
			}
			if (digits > 0) {
				if (count < max) {
					out[count] = value;
				}
				count++;
			}
			/* Skip the rest of a number that was refused. */
			while (*at >= '0' && *at <= '9') {
				at++;
			}
			continue;
		}
		if (*at == ',' || *at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
			at++;
			continue;
		}
		/* Anything else is not a list of integers any more. */
		break;
	}

	return (count > max) ? max : count;
}

/*
    A JSON array of *objects*, taking one numeric field out of each.

    `r=startsession` answers with a different shape from `r=unlocks`, and the difference is not
    cosmetic:

        "Unlocks":[{"ID":93119,"When":1786243173},{"ID":93120,"When":1786243180}]

    raNetJsonIdList() reads the first shape and would stop at the `{` here, returning nothing and
    calling it an empty list -- which is the failure this exists to avoid, since an empty list is a
    meaningful answer from that endpoint.

    Same contract as raNetJsonIdList(): **-1 for a missing key, 0 for an empty array**, truncation
    reported by returning `max`. Same refusal on overflow, for the same reason -- a truncated id is a
    different achievement.

    Deliberately not a JSON parser, and specific about what it will accept: it walks from `"key":[` to
    the matching `]`, taking `"field":<digits>` inside each object. It tracks brace depth so a nested
    object cannot smuggle a value in from a level this did not mean to read, and it stops at the array's
    own close rather than at the first `]` it sees.

    The limit, stated rather than discovered later: brace counting is not string-aware, so a `{`, `}` or
    `]` *inside a string value* in one of those objects would throw the depth off. That is safe for the
    two arrays this reads -- `Unlocks` and `HardcoreUnlocks` hold objects of two numbers, `ID` and
    `When`, and no strings at all. Pointing it at an array of objects with titles in them would need a
    real parser, and this should not be the function that gets reused there.
*/
int raNetJsonObjectField(const char* json, const char* key, const char* field, u32* out, int max) {
	char        needle[40];
	char        inner[40];
	const char* at;
	int         count = 0;
	int         depth = 0;

	if (sniprintf(needle, sizeof(needle), "\"%s\":[", key) >= (int)sizeof(needle)) {
		return -1;
	}
	if (sniprintf(inner, sizeof(inner), "\"%s\":", field) >= (int)sizeof(inner)) {
		return -1;
	}
	at = strstr(json, needle);
	if (!at) {
		return -1;
	}
	at += strlen(needle);

	while (*at) {
		if (*at == '{') {
			depth++;
			at++;
			continue;
		}
		if (*at == '}') {
			if (depth > 0) {
				depth--;
			}
			at++;
			continue;
		}
		if (*at == ']' && depth == 0) {
			break;
		}
		/*
		    Only at depth 1 -- directly inside one of the array's own objects. A deeper `"ID":` belongs
		    to something else, and reading it would quietly mix two levels of the reply together.
		*/
		if (depth == 1 && strncmp(at, inner, strlen(inner)) == 0) {
			u32 value  = 0;
			int digits = 0;

			at += strlen(inner);
			while (*at == ' ' || *at == '\t') {
				at++;
			}
			while (*at >= '0' && *at <= '9') {
				const u32 digit = (u32)(*at - '0');

				if (value > (0xFFFFFFFFu - digit) / 10u) {
					digits = 0;
					break;
				}
				value = value * 10 + digit;
				digits++;
				at++;
			}
			while (*at >= '0' && *at <= '9') {
				at++;
			}
			if (digits > 0) {
				if (count < max) {
					out[count] = value;
				}
				count++;
			}
			continue;
		}
		at++;
	}

	return (count > max) ? max : count;
}

#endif /* RA_LAUNCHER_WIFI */
