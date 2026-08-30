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
/*
    And the achievement's own id, which arrives *before* its MemAddr in each object -- the opposite
    of Flags, and the reason ids need no deferral while flags do.

    The needle includes the opening quote, and that is what keeps it from matching the fields that
    merely end in ID: `"GameID":` and `"ConsoleID":` both contain `ID":` but neither has a quote
    immediately before the `I`. The reply's root object does carry a bare `"ID"` -- the game's -- and
    it appears before any achievement, so the pending id is *cleared when a MemAddr consumes it*
    rather than left standing. Otherwise an achievement that arrived without an id of its own would
    silently inherit the game's, and be counted as having one.
*/
static const char raPatchSayId[]      = "\"ID\":";
/*
    And its Title, which arrives between the id and the MemAddr in each object. Captured for one
    reason: so the notification on screen can say *which* achievement was earned rather than that one
    was. See the record format at raPatch in ra_wifi.h.

    `"Title":"` has no borders -- no proper prefix is also a suffix except the quote at length one,
    which raPatchAdvance's retest already covers -- so the same matcher handles it unchanged.
*/
static const char raPatchSayTitle[]   = "\"Title\":\"";
/*
    ...and its Description and Points, for the in-game viewer rather than for the notification.

    Both sit between the id and the MemAddr in each object, exactly where the Title does, so both
    are read the same way and need no deferral.

    The same border argument holds for both needles, which is what makes raPatchAdvance's
    restart-and-retest exact rather than nearly exact. `"Description":"` repeats only `"` (at 0, 12
    and 14) and `i` (at 6 and 9), and neither gives a proper prefix of length two or more that is
    also a suffix of a longer one -- the character after each later `"` is `:` and then end, never
    `D`. `"Points":` repeats only `"`, at 0 and 7, and is followed by `:`.

    Points is the odd one out in this file: it is a JSON *number*, so unlike every other needle here
    it does not end at a closing quote. It is read to the first byte that is not a digit.
*/
static const char raPatchSayDesc[]    = "\"Description\":\"";
static const char raPatchSayPoints[]  = "\"Points\":";
/*
    And the game's Rich Presence script, which is measured rather than kept -- see raPatch::richBytes.

    Borders again, and again none of length two or more: `"RichPresencePatch":"` repeats the quote (at
    0, 18 and 20) and several letters, but the two-character prefix is `"R` and no later quote is
    followed by an `R`. So restart-and-retest stays exact with a fifth needle.
*/
static const char raPatchSayRich[]    = "\"RichPresencePatch\":\"";

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
    How many decimal digits an id needs, and the digits themselves. Written by hand rather than with
    sniprintf() because this file is deliberately free of stdio -- it runs in a launcher whose heap
    is already accounted for to the byte, and a decimal conversion is six lines.
*/
static u32 raPatchIdDigits(u32 id) {
	u32 digits = 1;
	u32 scale  = 10;

	while (id >= scale && digits < 10) {
		digits++;
		scale *= 10;
	}
	return digits;
}

static u32 raPatchWriteId(char* out, u32 id) {
	const u32 digits = raPatchIdDigits(id);
	u32       i      = digits;

	while (i > 0) {
		out[--i] = (char)('0' + (id % 10));
		id /= 10;
	}
	return digits;
}

/*
    Drop everything held for the definition being abandoned.

    Written once because it was written three times and one copy was wrong. The early exits -- an
    unofficial achievement, an odd id, one the account already holds -- each cleared the memaddr and
    the flags and *not* the title, while the only path that cleared the title was the one at the end
    of raPatchCommit(). So a definition following a discarded one inherited its label: RA sends
    unofficial achievements interleaved with published ones, so this was reachable on a real set and
    would have shown a player the wrong achievement's name on the notification.

    Invisible until the description and points arrived, because a wrong label is a wrong label and
    nothing compared it to anything. The viewer's fixture compared it to something.
*/
static void raPatchForget(raPatch* p) {
	p->pendingOpen   = 0;
	p->pendingBad    = 0;
	p->pendingSeen   = 0;
	p->pendingLength = 0;
	p->flagsSeen     = 0;
	p->flags         = 0;
	p->pendingId     = 0;
	p->titleLength   = 0;
	p->title[0]      = 0;
	p->titleFull     = 0;
	p->descLength    = 0;
	p->desc[0]       = 0;
	p->descFull      = 0;
	p->points        = 0;
	p->pointsSeen    = 0;
	p->pointsBad     = 0;
}

/*
    An achievement the account already holds, written for the viewer and for nothing else.

    The record is `#!<id>\t<title>\t<points>\t<description>\t<when>` -- **no memaddr at all**, which is
    what makes this nearly free. An armed definition is dominated by its memaddr: the largest real
    set averages over 500 bytes a line for them, against about 100 for everything a person reads.
    So an achievement moving from "to earn" to "earned" gives the block back four fifths of its
    space, and the worst case for the block is a set where nothing has been earned yet -- which is
    exactly the case that was already measured at 87% full.

    The `#` is not a new convention. ra_split_definitions() in cardenginei_arm9_ra has skipped lines
    beginning with one since it was written, so these are invisible to rcheevos without a single
    line changing over there -- no empty memaddr to refuse, no error counter to move, nothing armed
    that must not be. The `!` after it distinguishes these from a comment a person typed in a
    hand-written file, which is the only other thing a `#` line can be.

    Degrades the same way and in the same order as an armed definition, because the fields are the
    same fields. If not even the bare id fits, the record is dropped and counted: the viewer showing
    an incomplete list is a smaller failure than the block overflowing, and the count is what says
    which happened.
*/
static void raPatchWriteEarned(raPatch* p) {
	u32 idLength;
	u32 titleLength = 0;
	u32 pointsLength = 0;
	u32 descLength = 0;
	u32 whenValue = 0;
	u32 whenLength = 0;

	if (!p->block || !p->pendingId) {
		return;
	}
	idLength = raPatchIdDigits(p->pendingId) + 2;   /* the marker and the digits */

	if (p->titleLength) {
		titleLength = (u32)p->titleLength + 1;
	}
	if (titleLength && p->pointsSeen && !p->pointsBad) {
		pointsLength = raPatchIdDigits(p->points) + 1;
	}
	if (pointsLength && p->descLength) {
		descLength = (u32)p->descLength + 1;
	}
	/*
	    ...and when it was earned, which only the launcher can answer -- see raWifiEarnedWhen().

	    Positional like every field before it, so it needs the description to be there: without it the
	    date would land at tab 3 and be read as a description. That is the same rule the chain above
	    already follows, one link further along.
	*/
	if (descLength && p->whenOf) {
		whenValue = p->whenOf(p->pendingId);
		if (whenValue) {
			whenLength = raPatchIdDigits(whenValue) + 1;
		}
	}

	p->wanted += idLength + titleLength + pointsLength + descLength + whenLength + 1;

	if (p->used + idLength + 1 <= p->blockMax) {
		u32* const trim[4] = { &whenLength, &descLength, &pointsLength, &titleLength };
		u16* const count[4] = { 0, &p->descNoRoom, &p->pointsNoRoom, &p->titleNoRoom };
		int        i;

		/*
		    The date goes first when the block is tight, and it is the only one of the four whose
		    loss is not counted: the other three are text a person came here to read, and this is a
		    line the page simply omits.
		*/
		for (i = 0; i < 4; i++) {
			if (p->used + idLength + titleLength + pointsLength + descLength + whenLength + 1
			    <= p->blockMax) {
				break;
			}
			if (*trim[i]) {
				*trim[i] = 0;
				if (count[i]) {
					(*count[i])++;
				}
			}
		}
	}

	if (p->used + idLength + titleLength + pointsLength + descLength + whenLength + 1
	    > p->blockMax) {
		p->earnedNoRoom++;
		return;
	}

	p->block[p->used++] = '#';
	p->block[p->used++] = '!';
	p->used += raPatchWriteId(p->block + p->used, p->pendingId);
	if (titleLength) {
		p->block[p->used++] = '\t';
		memcpy(p->block + p->used, p->title, p->titleLength);
		p->used += p->titleLength;
	}
	if (pointsLength) {
		p->block[p->used++] = '\t';
		p->used += raPatchWriteId(p->block + p->used, p->points);
	}
	if (descLength) {
		p->block[p->used++] = '\t';
		memcpy(p->block + p->used, p->desc, p->descLength);
		p->used += p->descLength;
	}
	if (whenLength) {
		p->block[p->used++] = '\t';
		p->used += raPatchWriteId(p->block + p->used, whenValue);
	}
	p->block[p->used++] = '\n';
	p->block[p->used]   = 0;
	p->earned++;
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
	u32 idLength = 0;
	u32 titleLength = 0;
	u32 pointsLength = 0;
	u32 descLength = 0;

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

		/*
		    The id's own digits count toward the block, because they are in the block. Formatted
		    here rather than measured separately so `wanted` stays the one number that answers
		    "would a complete set have fit".
		*/
		/*
		    Not an achievement, and now that is known rather than suspected.

		    The capture this scanner takes around such an id came back reading
		    `"Title":"Warning: Unknown Emulator","Description":"Hardcore unlocks cannot be earned
		    using this emulator.","MemAddr":"1=1.300.","Points":0,"Author":""`. **The server injects
		    it** -- it is a message to the player wearing an achievement's clothes, delivered as
		    always-true-after-300-frames so that a normal RA client pops it up. Which also retires the
		    theory that a flat scan was reading across this game's subsets: the entry is in
		    `Achievements` with `Flags` 3 because RetroAchievements put it there.

		    So it is dropped rather than staged. It is not something a player earned, its Points are
		    zero, its Author is empty, and staging it spent 19 bytes of an 88%-full block on an
		    entry that step 6 would then try to award on every boot. Counted and its context logged,
		    so the server's message reaches the log even though the definition does not reach the
		    game.
		*/
		if (p->pendingId >= RA_ODD_ID_FROM) {
			p->oddIds++;
			if (p->oddId == 0) {
				p->oddId = p->pendingId;
			}
			raPatchForget(p);
			return;
		}
		/*
		    Already earned, so it is left out. Checked here rather than in the cardengine because the
		    block is the scarce thing -- see raPatch.skipIds.
		*/
		if (p->pendingId && p->skipIds) {
			u16 i;

			for (i = 0; i < p->skipCount; i++) {
				if (p->skipIds[i] == p->pendingId) {
					p->alreadyDone++;
					raPatchWriteEarned(p);
					raPatchForget(p);
					return;
				}
			}
		}

		if (p->pendingId) {
			p->withId++;
			idLength = raPatchIdDigits(p->pendingId) + 1;   /* the digits and the colon */
		} else {
			p->withoutId++;
			idLength = 0;
		}

		/*
		    The title costs a tab and its own bytes. Counted into `wanted` with everything else, so the
		    log's block accounting stays a measurement of what the set actually needs.
		*/
		if (p->titleLength) {
			titleLength = (u32)p->titleLength + 1;   /* the tab and the text */
		}

		/*
		    Points and description cost a tab each plus their own bytes, and neither can be written
		    without the field before it: the record is positional and every valid form is a prefix
		    ending at a tab boundary. See raPatch in ra_wifi.h for the table.
		*/
		if (titleLength && p->pointsSeen && !p->pointsBad) {
			pointsLength = raPatchIdDigits(p->points) + 1;
		}
		if (pointsLength && p->descLength) {
			descLength = (u32)p->descLength + 1;
		}

		/* id, memaddr, title, points, description, and the newline */
		p->wanted += idLength + length + titleLength + pointsLength + descLength + 1;

		/*
		    If the label is what does not fit, drop the label and keep the achievement.

		    Without this, adding titles would silently turn working achievements off at the end of a
		    large set: the block runs 88% full on a real one, and a definition that fitted yesterday
		    would be `dropped` today for the sake of thirty bytes of text. An achievement with no
		    label still unlocks, still reports to the server, and still shows a notification -- just a
		    nameless one. That trade only goes one way.

		    Three fields deep now, and it degrades in one direction: **description, then points, then
		    title**, which is the record read right to left. That order is not a preference either.
		    The description is up to 64 bytes and points is at most 5, so dropping the cheap field
		    first would buy almost nothing and cost the same achievement its label a moment later.
		    And the record is positional, so a field can only be dropped if everything after it is
		    dropped too -- which is exactly why the expensive one was put last.

		    Written as a loop over the three lengths rather than as three nested tests, because the
		    question at each step is the same one and stating it three times is how the second and
		    third get subtly different bounds.
		*/
		/*
		    Only when trimming can actually save the achievement. Without the outer test a definition
		    that was going to be dropped anyway would still have its label counted as "dropped for
		    room", which is a different fact and the one the counter exists to report -- the old
		    single-field guard was precise about this and the loop has to stay precise too.
		*/
		if (p->block && p->used + idLength + length + 1 <= p->blockMax) {
			u32* const trim[3] = { &descLength, &pointsLength, &titleLength };
			u16* const count[3] = { &p->descNoRoom, &p->pointsNoRoom, &p->titleNoRoom };
			int        i;

			for (i = 0; i < 3; i++) {
				if (p->used + idLength + length
				    + titleLength + pointsLength + descLength + 1 <= p->blockMax) {
					break;
				}
				if (*trim[i]) {
					*trim[i] = 0;
					(*count[i])++;
				}
			}
		}

		/*
		    Zero is the unset marker rather than a counter, and it is safe as one: an empty value
		    is filtered above, so no definition that reaches here has length 0.
		*/
		if (p->shortest == 0 || length < p->shortest) {
			p->shortest = length;
		}

		if (p->block && p->used + idLength + length
		    + titleLength + pointsLength + descLength + 1 <= p->blockMax) {
			if (idLength) {
				p->used += raPatchWriteId(p->block + p->used, p->pendingId);
				p->block[p->used++] = ':';
			}
			memcpy(p->block + p->used, p->pending, length);
			p->used += length;
			/*
			    After the memaddr, never before it, and that is the whole reason a tab works as the
			    delimiter: a reader that knows nothing about titles finds the memaddr exactly where it
			    always was and stops at the first character that cannot be part of one.
			*/
			if (titleLength) {
				p->block[p->used++] = '\t';
				memcpy(p->block + p->used, p->title, p->titleLength);
				p->used += p->titleLength;
				p->withTitle++;
			}
			if (pointsLength) {
				p->block[p->used++] = '\t';
				p->used += raPatchWriteId(p->block + p->used, p->points);
				p->withPoints++;
			}
			if (descLength) {
				p->block[p->used++] = '\t';
				memcpy(p->block + p->used, p->desc, p->descLength);
				p->used += p->descLength;
				p->withDesc++;
			}
			p->block[p->used++] = '\n';
			p->block[p->used]   = 0;
			p->kept++;
		} else {
			p->dropped++;
		}
	}

	/*
	    Consumed, not carried -- every field of it. See raPatchSayId: the reply's root object has an
	    "ID" of its own and it precedes every achievement, so a definition that arrived without one
	    must read as id-less rather than inherit the game's. The same is true of every label.
	*/
	raPatchForget(p);
}

/*
    One decoded byte of the title being read.

    Truncates and says so, rather than refusing the definition the way an over-long memaddr is
    refused. The asymmetry is the point: a clipped memaddr is a *different achievement* and must never
    be kept, while a clipped title is the same achievement with a shorter label.
*/
/*
    One decoded byte of the description being read.

    A copy of raPatchTitleByte() rather than a shared helper taking a buffer, a length, a limit and
    two counters: the parameter list would be longer than the body, and this file runs in a launcher
    whose every byte of heap is already accounted for.
*/
static void raPatchDescByte(raPatch* p, char c) {
	if (p->descLength < RA_PATCH_DESC_MAX - 1) {
		p->desc[p->descLength++] = c;
		p->desc[p->descLength]   = 0;
	} else if (!p->descFull) {
		p->descFull = 1;
		p->descCut++;
	}
}

static void raPatchTitleByte(raPatch* p, char c) {
	if (p->titleLength < RA_PATCH_TITLE_MAX - 1) {
		p->title[p->titleLength++] = c;
		p->title[p->titleLength]   = 0;
	} else if (!p->titleFull) {
		/*
		    A flag of its own rather than letting titleLength run past the buffer as a marker: commit
		    copies titleLength bytes, so a length used as a sentinel would be a length that reads one
		    byte past the text. Counted once per title, not once per dropped byte -- the question the
		    log answers is how many labels were clipped, not by how much.
		*/
		p->titleFull = 1;
		p->titleCut++;
	}
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

		if (p->oddCapture && p->oddFill < RA_ODD_CONTEXT_MAX - 1) {
			p->oddContext[p->oddFill++] = c;
			p->oddContext[p->oddFill]   = 0;
			if (p->oddFill >= RA_ODD_CONTEXT_MAX - 1) {
				p->oddCapture = 0;
			}
		}

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
				p->state    = RA_PATCH_SCAN;
				p->inString = 0;
				p->memAt    = 0;
				p->flagsAt  = 0;
				p->idAt     = 0;
				continue;
			}
			raPatchPending(p, c);
			continue;
		}

		if (p->state == RA_PATCH_TITLE) {
			if (p->escape) {
				p->escape = 0;
				/*
				    The same three unescaped as a memaddr's, and everything else keeps its backslash --
				    which is what guarantees the record's delimiters can never appear in a title. JSON
				    cannot carry a raw tab or newline inside a string; they arrive as `\t` and `\n`, and
				    this puts back a literal backslash and letter rather than a control character.

				    A `\u` escape comes out as its own text for the same reason it does for a memaddr:
				    inventing a decoder for one would be inventing a bug, and the cost here is a title
				    with an odd-looking accent rather than a definition that will not parse.
				*/
				if (c == '/' || c == '\\' || c == '"') {
					raPatchTitleByte(p, c);
				} else {
					raPatchTitleByte(p, '\\');
					raPatchTitleByte(p, c);
				}
				continue;
			}
			if (c == '\\') {
				p->escape = 1;
				continue;
			}
			if (c == '"') {
				p->state    = RA_PATCH_SCAN;
				p->inString = 0;
				p->memAt    = 0;
				p->flagsAt  = 0;
				p->idAt     = 0;
				p->titleAt  = 0;
				continue;
			}
			raPatchTitleByte(p, c);
			continue;
		}

		if (p->state == RA_PATCH_DESC) {
			/* Byte for byte the title's rules; see the note there for why escapes stay escaped. */
			if (p->escape) {
				p->escape = 0;
				if (c == '/' || c == '\\' || c == '"') {
					raPatchDescByte(p, c);
				} else {
					raPatchDescByte(p, '\\');
					raPatchDescByte(p, c);
				}
				continue;
			}
			if (c == '\\') {
				p->escape = 1;
				continue;
			}
			if (c == '"') {
				p->state    = RA_PATCH_SCAN;
				p->inString = 0;
				p->memAt    = 0;
				p->flagsAt  = 0;
				p->idAt     = 0;
				p->titleAt  = 0;
				p->descAt   = 0;
				p->pointsAt = 0;
				continue;
			}
			raPatchDescByte(p, c);
			continue;
		}

		if (p->state == RA_PATCH_RICH) {
			/*
			    Decoded length, not raw: an escape is two bytes on the wire and one or two after
			    decoding, and what matters is how much would have to be *stored*. Same rule the title
			    and the description use, so the number is comparable to theirs.
			*/
			if (p->escape) {
				p->escape = 0;
				p->richBytes += (c == '/' || c == '\\' || c == '"') ? 1u : 2u;
				continue;
			}
			if (c == '\\') {
				p->escape = 1;
				continue;
			}
			if (c == '"') {
				p->state    = RA_PATCH_SCAN;
				p->inString = 0;
				p->memAt    = 0;
				p->flagsAt  = 0;
				p->idAt     = 0;
				p->titleAt  = 0;
				p->descAt   = 0;
				p->pointsAt = 0;
				p->richAt   = 0;
				continue;
			}
			p->richBytes++;
			continue;
		}

		if (p->state == RA_PATCH_POINTS) {
			if (c >= '0' && c <= '9') {
				const u32 digit = (u32)(c - '0');

				/*
				    Refused on overflow rather than clamped, the same correction the id needed. A
				    points value that will not fit a u32 is not a points value, and a clamped one
				    would be a number this fork invented.
				*/
				if (p->points > 0xFFFFFFFFu / 10u
				 || (p->points == 0xFFFFFFFFu / 10u && digit > 0xFFFFFFFFu % 10u)) {
					p->pointsBad = 1;
				}
				if (!p->pointsBad) {
					p->points = p->points * 10u + digit;
				}
				p->pointsSeen = 1;
				continue;
			}
			/*
			    Anything that is not a digit ends the number -- and the byte is *reprocessed* rather
			    than swallowed, because unlike every other field here this one has no closing quote.
			    The character that ends it is a comma or a brace, which is a byte the scanner has to
			    see: a swallowed `{` is a pending id that never gets cleared.
			*/
			p->state    = RA_PATCH_SCAN;
			p->memAt    = 0;
			p->flagsAt  = 0;
			p->idAt     = 0;
			p->titleAt  = 0;
			p->descAt   = 0;
			p->pointsAt = 0;
			/* fall through to the scan state with this same byte */
		}

		if (p->state == RA_PATCH_ID) {
			if (c >= '0' && c <= '9') {
				const u32 digit = (u32)(c - '0');

				/*
				    Refused on overflow, not clamped -- and this is a correction rather than a
				    precaution. The first version stopped accumulating above 100,000,000 on the
				    reasoning that "RA ids are six or seven digits today", and then the real set for
				    GameID 14856 turned up with **101000001** on its first line. That one happens to
				    survive the clamp; a ten-digit id, which a u32 holds perfectly well, would have
				    come out one digit short and named a different achievement. Silently.
				
				    So a value that will not fit sets the sticky bad flag and the definition ends up
				    counted in withoutId. No id is a definition that cannot be reported; a wrong id
				    is an unlock awarded to somebody else's achievement.
				*/
				if (p->idBad) {
					continue;
				}
				if (p->pendingId > (0xFFFFFFFFu - digit) / 10u) {
					p->idBad     = 1;
					p->pendingId = 0;
					continue;
				}
				p->pendingId = p->pendingId * 10 + digit;
				continue;
			}
			p->idBad = 0;
			/*
			    The digits are in. If this id is one of the ones nobody can explain, start copying
			    the reply verbatim from here -- the fields that would identify it (Title, Points,
			    Flags, Type) all follow the id in the object, so this is the only place they can be
			    caught without holding a reply that does not fit.
			*/
			if (p->pendingId >= RA_ODD_ID_FROM && p->oddFill == 0) {
				p->oddCapture = 1;
			}
			/*
			    Falls through to the scanner with the same character, for the reason the Flags state
			    does: what ends the digits could be the quote that opens the next key.
			*/
			p->state   = RA_PATCH_SCAN;
			p->memAt   = 0;
			p->flagsAt = 0;
			p->idAt    = 0;
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
			p->idAt    = 0;
		}

		/*
		    An id belongs to the object it was written in, and `{` is where an object starts.

		    Clearing there is what makes the *first* achievement correct, and nothing else would.
		    The reply opens `{"Success":true,"PatchData":{"ID":14856,...` -- the game's id -- and
		    then `"Achievements":[{"ID":1,"MemAddr":...`. Without this, an achievement that arrived
		    with no id of its own would inherit whatever preceded it, which for the first one is the
		    **game's** id. That is worse than having none: a wrong id reports an unlock for an
		    achievement the player did not earn, where a missing one reports nothing.

		    **And it has to know when it is inside a string**, which is a correction. This used to fire
		    on every `{`, justified by "ID comes first in each object and MemAddr immediately after, so
		    there is no text between them for a stray brace to sit in". That is not the order RA sends:
		    the one reply this project has captured reads
		    `"ID":101000001,"Title":"...","Description":"Hardcore unlocks cannot be earned using this
		    emulator.","MemAddr":"1=1.300.","Points":0,"Author":""`. A Description sits between the id
		    and the memaddr, an Author sits between the memaddr and the Flags field that commits, and a
		    brace in either would have cleared an id that was perfectly good -- leaving an achievement
		    that cannot be reported to the server at all, silently.

		    So the scanner tracks quoting for this one purpose. Only the brace rule consults it; the
		    needles run over every byte exactly as before, because that is what makes them safe against
		    a key name appearing inside a value.
		*/
		if (p->scanEscape) {
			p->scanEscape = 0;
		} else if (c == '\\') {
			p->scanEscape = 1;
		} else if (c == '"') {
			p->inString = !p->inString;
		} else if (c == '{' && !p->inString) {
			p->pendingId = 0;
		}

		p->memAt    = raPatchAdvance(raPatchSayMemAddr, p->memAt, c);
		p->flagsAt  = raPatchAdvance(raPatchSayFlags, p->flagsAt, c);
		p->idAt     = raPatchAdvance(raPatchSayId, p->idAt, c);
		p->titleAt  = raPatchAdvance(raPatchSayTitle, p->titleAt, c);
		p->descAt   = raPatchAdvance(raPatchSayDesc, p->descAt, c);
		p->pointsAt = raPatchAdvance(raPatchSayPoints, p->pointsAt, c);
		p->richAt   = raPatchAdvance(raPatchSayRich, p->richAt, c);

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
			p->idAt        = 0;
			p->titleAt     = 0;
			p->descAt      = 0;
			p->pointsAt    = 0;
			continue;
		}
		if (raPatchSayFlags[p->flagsAt] == 0) {
			p->state     = RA_PATCH_FLAGS;
			p->flags     = 0;
			p->flagsSeen = 0;
			p->memAt     = 0;
			p->flagsAt   = 0;
			p->idAt      = 0;
			p->titleAt   = 0;
			p->descAt    = 0;
			p->pointsAt  = 0;
			continue;
		}
		if (raPatchSayId[p->idAt] == 0) {
			/*
			    No commit here, unlike the MemAddr needle. An id belongs to the definition that is
			    about to arrive, not to the one being held -- which is the whole reason ids are
			    simpler than flags.
			*/
			p->state     = RA_PATCH_ID;
			p->pendingId = 0;
			p->idBad     = 0;
			/*
			    A fresh id begins, so any capture in progress is for the previous one and is done.
			    Only the first odd id is captured -- one example is what identifies the shape.
			*/
			p->oddCapture = 0;
			p->memAt     = 0;
			p->flagsAt   = 0;
			p->idAt      = 0;
			p->titleAt   = 0;
			p->descAt    = 0;
			p->pointsAt  = 0;
		}
		if (raPatchSayTitle[p->titleAt] == 0) {
			/*
			    No commit here either, and for the id's reason: a title belongs to the definition
			    about to arrive. Started fresh rather than appended to, so a second Title key in one
			    object -- which is not something RA sends, but is something a proxy could -- replaces
			    the label instead of concatenating two.
			*/
			p->state       = RA_PATCH_TITLE;
			p->titleLength = 0;
			p->title[0]    = 0;
			p->titleFull   = 0;
			p->escape      = 0;
			p->memAt       = 0;
			p->flagsAt     = 0;
			p->idAt        = 0;
			p->titleAt     = 0;
			p->descAt      = 0;
			p->pointsAt    = 0;
			continue;
		}
		if (raPatchSayDesc[p->descAt] == 0) {
			/* The title's rules exactly; see the note on that branch. */
			p->state      = RA_PATCH_DESC;
			p->descLength = 0;
			p->desc[0]    = 0;
			p->descFull   = 0;
			p->escape     = 0;
			p->memAt      = 0;
			p->flagsAt    = 0;
			p->idAt       = 0;
			p->titleAt    = 0;
			p->descAt     = 0;
			p->pointsAt   = 0;
			continue;
		}
		if (raPatchSayRich[p->richAt] == 0) {
			/*
			    No commit, and no interaction with the definition being held: this key belongs to the
			    reply's root object, not to an achievement, and it may arrive anywhere relative to
			    them. Counting is all it does, so it cannot disturb one.
			*/
			p->state     = RA_PATCH_RICH;
			p->richBytes = 0;
			p->richSeen  = 1;
			p->escape    = 0;
			p->memAt     = 0;
			p->flagsAt   = 0;
			p->idAt      = 0;
			p->titleAt   = 0;
			p->descAt    = 0;
			p->pointsAt  = 0;
			p->richAt    = 0;
			continue;
		}
		if (raPatchSayPoints[p->pointsAt] == 0) {
			p->state      = RA_PATCH_POINTS;
			p->points     = 0;
			p->pointsSeen = 0;
			p->pointsBad  = 0;
			p->memAt      = 0;
			p->flagsAt    = 0;
			p->idAt       = 0;
			p->titleAt    = 0;
			p->descAt     = 0;
			p->pointsAt   = 0;
			continue;
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
