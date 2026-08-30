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
    One tab-delimited field of a record, NUL-terminated into `out`, which holds `max` characters plus
    the terminator. Returns where reading stopped -- at the delimiter, never past it.

    The whole field is *consumed* even when it is longer than `max`; only the copy is clipped. Stopping
    early would leave the tail of a title in front of the parser, and the tail of a title is very often
    a digit that would then be read as an achievement id.

    Anything outside printable ASCII is a terminator, so the record's NUL padding ends the last field
    without needing to know how long the record is -- and a ROM header that is not text cannot put a
    control character into a format whose fields are delimited by them. The cardengine copies those two
    header fields out raw precisely because this end does the tidying; see raUnlockAppend().
*/
static int takeField(const char* text, int at, int length, char* out, int max) {
	int n = 0;

	while (at < length && text[at] != '\t'
	    && (unsigned char)text[at] >= ' ' && (unsigned char)text[at] <= '~') {
		if (n < max) {
			out[n++] = text[at];
		}
		at++;
	}
	/* Trailing spaces: gameTitle is a fixed twelve bytes in the header and is padded with them. */
	while (n > 0 && out[n - 1] == ' ') {
		n--;
	}
	out[n] = 0;
	return at;
}

/*
    Step over the NUL padding *inside* a fixed-width field, so the next delimiter can be found.

    This exists because of a bug that reached a real account, and the shape of it is worth keeping.
    raUnlockAppend() writes `gameCode` and `gameTitle` as fixed 4 and 12 bytes straight out of the ROM
    header -- deliberately, because trimming them costs bytes the ARM7 does not have. A header pads
    with NULs as often as with spaces, and `CONTRA 4` is eight characters in a twelve-byte field. So
    the record on the card is

        302329<tab>20260815183300<tab>YCTE<tab>CONTRA 4\0\0\0\0<tab>1<newline>

    takeField() stops at the first NUL, which is four bytes short of the tab. That was harmless for as
    long as the title was the *last* field: nothing followed, so stopping early lost nothing. The
    moment a field was appended after it, the parser looked for a tab, found a NUL, gave up -- and
    left the mode digit in front of the outer loop, which read `1` as an achievement id and submitted
    it. The server accepted it: `AchievementID:1`, from a game the player has never run.

    Only NULs are skipped, and that bound is what makes it safe. Stopping at anything else means a
    record with no mode field lands on its newline, and a record whose padding runs to the end of the
    record lands on the next record's first digit -- which the outer loop then reads as an id, exactly
    as it should. Skipping *any* non-printable, or scanning ahead for a tab, would swallow the record
    after this one. That is the same failure the stamp's own comment describes, and it is the reason
    this is a skip rather than a search.
*/
static int skipPadding(const char* text, int at, int length) {
	while (at < length && text[at] == 0) {
		at++;
	}
	return at;
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
		u32  value;
		u32  stamp;
		u32 packed;
		char code[RA_QUEUE_CODE + 1];
		char title[RA_QUEUE_TITLE + 1];
		int hardcore;
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
		stamp  = 0;
		packed = 0;
		code[0] = 0;
		title[0] = 0;
		hardcore = 0;
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
				stamp  = raQueueStampToUnix(text + start);
				packed = raWhenFromStamp(text + start);
			}

			/*
			    Then the game: its code, then its title, each running to the next tab or to the end of
			    the record.

			    **Consuming these correctly is not bookkeeping, it is the whole safety of the format.**
			    A DS game title routinely ends in a digit -- `CONTRA4` does -- and a title left
			    unconsumed would have that `4` read as an achievement id and submitted as an unlock the
			    player never earned. So these run to their delimiter whatever they contain, which is
			    also why the delimiter is a character no field can hold.
			*/
			/*
			    Each field is followed by skipPadding(), because the writer's fields are fixed width
			    and its padding is NUL. Without it the search for the next tab starts on the padding
			    and fails, and every field after this one is lost -- silently, since a lost field
			    reads exactly like a record that never had one.
			*/
			i = skipPadding(text, i, length);
			if (i < length && text[i] == '\t') {
				i++;
				i = takeField(text, i, length, code, RA_QUEUE_CODE);
				i = skipPadding(text, i, length);
				if (i < length && text[i] == '\t') {
					i++;
					i = takeField(text, i, length, title, RA_QUEUE_TITLE);
					i = skipPadding(text, i, length);

					/*
					    And the mode, which is one character and must be consumed for the same reason
					    the title must: left behind, the outer loop reads `1` as an achievement id
					    and submits an unlock the player never earned. That is not a hypothetical --
					    it happened, to a real account, on the first hardware run of this field, and
					    the padding above is why.

					    Anything that is not `1` is softcore, including a missing field, a truncated
					    record and a character a hand edit put there. Only `1` is a claim, and a
					    claim has to be written to be made.
					*/
					if (i < length && text[i] == '\t') {
						i++;
						if (i < length && text[i] != '\n' && text[i] != 0) {
							hardcore = (text[i] == '1');
							i++;
						}
					}
				}
			}

			/*
			    And then to the end of the line, whatever is left.

			    **A record that carried a stamp is a machine-written record, and nothing else on its
			    line is an achievement id.** That follows from the format rather than from a guess: a
			    tab after the id is the thing no hand-typed list could contain, and it is already what
			    licenses reading the next fourteen digits as a date instead of an unlock.

			    This is the guard that would have made the padding bug harmless rather than expensive.
			    A field the parser fails to reach -- because a writer padded it, or because a later
			    version added one this build does not know about -- is discarded here instead of being
			    handed to the outer loop as a number. Measured, not asserted: with skipPadding()
			    disabled, the suite's hardware-shaped records still parse as the right *count* with
			    nothing fabricated, and lose only the mode value they could not reach.

			    The forgiving behaviour is kept exactly where it is wanted. A line with no tab is still
			    a list of bare ids, which is what makes the file hand-writable and what let the sending
			    half be tested before the writing half existed.

			    Two gates for one failure, and this is the one worth having: consuming each field
			    correctly is a rule every future field has to re-earn, while stopping at the newline is
			    a rule that already holds for fields nobody has written yet.
			*/
			while (i < length && text[i] != '\n') {
				i++;
			}
		}

		/*
		    A synthetic id is not an achievement, and here rather than three lines earlier because
		    the record's stamp, code and title have to be consumed first: dropping at the id would
		    leave `20260815183256` and `PICROSS3D` to be read as two more ids.

		    cardenginei_arm9_ra no longer writes these -- see ra_rc_queue_unlock() -- but every card
		    that ran a build which did is carrying one per boot of every game the server does not
		    know, and each one is a `404 Unknown achievement` sent on the next boot. Dropping them on
		    the way in means they are never sent and raQueuePack() writes them out of the file, so a
		    card cleans itself on the first boot with wifi.

		    Not `dropped`: these are well-formed records that meant something to the build that wrote
		    them, and a count that says "cleaned one" reads differently from one that says "your file
		    is corrupt".
		*/
		if (value >= RA_SYNTHETIC_ID_BASE) {
			q->synthetic++;
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
		q->times[q->count] = stamp;
		q->packed[q->count] = packed;
		if (stamp) {
			q->stamped++;
		}
		strcpy(q->codes[q->count], code);
		strcpy(q->titles[q->count], title);
		if (code[0]) {
			q->named++;
		}
		q->hardcore[q->count] = (u8)(hardcore ? 1 : 0);
		if (hardcore) {
			q->hard++;
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
int raQueuePack(const raQueue* q, const int* keep, int keepCount, char* out, int outSize) {
	int written = 0;
	int i;

	if (outSize < RA_QUEUE_RECORD || (outSize % RA_QUEUE_RECORD) != 0) {
		return -1;
	}
	if (keepCount < 0 || keepCount > outSize / RA_QUEUE_RECORD) {
		return -1;
	}

	memset(out, 0, (size_t)outSize);

	for (i = 0; i < keepCount; i++) {
		char*     record = out + (i * RA_QUEUE_RECORD);
		const int from = keep[i];
		char      digits[11];
		int       n = 0;
		int       at;
		u32       value;
		u32       when;

		if (from < 0 || from >= q->count) {
			return -1;
		}
		value = q->ids[from];
		when  = q->times[from];
		if (value == 0) {
			continue;
		}
		while (value && n < (int)sizeof(digits)) {
			digits[n++] = (char)('0' + (value % 10u));
			value /= 10u;
		}

		/*
		    Measured before a byte is written, not after. `record` points into a buffer the caller
		    sized, so a record that did not fit would not fail a check -- it would run into the next
		    record, and on the last one past the end of the buffer entirely.

		    A u32 is at most 10 digits and the longest record is 10+1+14+1+4+1+12+1+1+1 = 46 of the 48
		    a record holds, so this cannot trigger. It is here because the record size is a tunable
		    and the failure it would otherwise cause is silent memory corruption.
		*/
		{
			int need = n + 1;  /* the id and its newline */

			if (when) {
				need += 1 + RA_QUEUE_STAMP;
				if (q->codes[from][0]) {
					need += 1 + (int)strlen(q->codes[from])
					      + 1 + (int)strlen(q->titles[from])
					      + 2;  /* the tab and the mode digit */
				}
			}
			if (need > RA_QUEUE_RECORD) {
				return -1;
			}
		}

		for (at = 0; at < n; at++) {
			record[at] = digits[n - 1 - at];
		}

		/*
		    Each field only appears if the one before it does, because they are positional: a title
		    with no code in front of it would be read as the code. A record that lost its stamp
		    somewhere therefore also drops its game, which is the honest outcome -- the alternative is
		    inventing a delimiter position that the writer never wrote.
		*/
		if (when) {
			record[at++] = '\t';
			raQueueUnixToStamp(when, record + at);
			at += RA_QUEUE_STAMP;

			if (q->codes[from][0]) {
				int k;

				record[at++] = '\t';
				for (k = 0; q->codes[from][k]; k++) {
					record[at++] = q->codes[from][k];
				}
				record[at++] = '\t';
				for (k = 0; q->titles[from][k]; k++) {
					record[at++] = q->titles[from][k];
				}

				/*
				    And the mode back out, because a record that is kept must not change what it
				    claims. A hardcore unlock the server never answered is still a hardcore unlock
				    next boot, and dropping the field here would quietly downgrade every record that
				    survived a failed submission -- the one population most likely to be retried.
				*/
				record[at++] = '\t';
				record[at++] = q->hardcore[from] ? '1' : '0';
			}
		}
		record[at++] = '\n';
		written++;
	}
	return written;
}

void raQueueTally(const raQueue* q, u32 now, raPendingBlock* out) {
	int i;

	memset(out, 0, sizeof(*out));
	out->magic = RA_PENDING_MAGIC;

	for (i = 0; i < q->count; i++) {
		raPendingGame* g = 0;
		int            k;

		out->total++;

		/*
		    A record with no game cannot be grouped with anything -- it is a bare id from a
		    hand-edited file or from a build older than the game field. Counted separately rather
		    than filed under an empty name, so the menu can say "1 unlock of unknown origin" instead
		    of inventing a row with a blank title.
		*/
		if (q->codes[i][0] == 0) {
			out->unnamed++;
			if (out->queuedCount < RA_PENDING_QUEUED_MAX) {
				out->queuedWhen[out->queuedCount] = q->packed[i];
				out->queued[out->queuedCount++]   = q->ids[i];
			}
			continue;
		}

		for (k = 0; k < out->games; k++) {
			if (strcmp(out->game[k].code, q->codes[i]) == 0) {
				g = &out->game[k];
				break;
			}
		}
		if (!g) {
			if (out->games >= RA_PENDING_GAMES_MAX) {
				/*
				    Said rather than silently folded away. A queue spanning more than eight games
				    is not a real scenario, but a screen that quietly under-reports what is owed
				    would be worse than one that admits it ran out of rows.
				*/
				out->dropped++;
				continue;
			}
			g = &out->game[out->games++];
			strcpy(g->code, q->codes[i]);
			strcpy(g->title, q->titles[i]);
		}

		g->count++;

		/*
		    ...and the id itself, so the achievements page can mark this one row rather than only
		    report a total. Recorded for every record, not only the named ones: an id with no game
		    is still an id the viewer may be showing.
		*/
		if (out->queuedCount < RA_PENDING_QUEUED_MAX) {
			out->queuedWhen[out->queuedCount] = q->packed[i];
			out->queued[out->queuedCount++]   = q->ids[i];
		}

		/*
		    The oldest of this game's unlocks, in whole days. Guarded both ways: a record with no
		    stamp has no age to contribute, and a clock that has moved backwards since the unlock
		    would make the subtraction wrap. Neither may report as zero days -- the menu prints that
		    as "today", which is a claim rather than an absence.
		*/
		if (q->times[i] && now >= q->times[i]) {
			const u32 days = (now - q->times[i]) / 86400u;

			if (days > 0xFFFFu) {
				g->waitDays = 0xFFFF;
			} else if ((u16)days > g->waitDays) {
				g->waitDays = (u16)days;
			}
		}
	}
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

/*
    Pack a date and time into the 27 bits raViewerEntry.when holds -- see RA_WHEN_MINUTE.

    Refuses anything out of range rather than wrapping it, and that is the whole of the validation
    policy here: a wrong date shown confidently is worse than no date, and the menu prints nothing
    for zero. Both sources feeding this are external -- a stamp written by the ARM7 into a file a
    person can edit, and a timestamp from a server -- so neither is trusted to be sane.

    Years before 2000 are refused as well as years past 2127. A DS with a dead RTC reports 2000 or
    earlier, and an achievement dated 1970 on the page would say nothing true.
*/
u32 raWhenPack(int year, int month, int day, int hour, int minute) {
	if (year < 2000 || year > 2127
	 || month < 1 || month > 12
	 || day < 1 || day > 31
	 || hour < 0 || hour > 23
	 || minute < 0 || minute > 59) {
		return 0;
	}
	return (u32)(((u32)(year - 2000) << 20)
	           | ((u32)month  << 16)
	           | ((u32)day    << 11)
	           | ((u32)hour   << 6)
	           | (u32)minute);
}

/*
    ...and from the queue file's own `YYYYMMDDhhmmss`, which is how a locally earned unlock carries
    its time. Seconds are read and dropped: the field this packs into has no room for them, and a
    minute is the resolution a person reading a menu can use.
*/
u32 raWhenFromStamp(const char* stamp) {
	int field[6] = { 0, 0, 0, 0, 0, 0 };
	int widths[6] = { 4, 2, 2, 2, 2, 2 };
	int at = 0;
	int f;

	if (!stamp) {
		return 0;
	}
	for (f = 0; f < 6; f++) {
		int i;

		for (i = 0; i < widths[f]; i++) {
			const char c = stamp[at++];

			if (c < '0' || c > '9') {
				return 0;
			}
			field[f] = field[f] * 10 + (c - '0');
		}
	}
	return raWhenPack(field[0], field[1], field[2], field[3], field[4]);
}
