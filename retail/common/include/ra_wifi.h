/*
    Step two of the network ladder: bring the Atheros chip up from inside nds-bootstrap's
    launcher, and report how it came up.

    Step one -- tools/wifiprobe/ -- already reached RetroAchievements over plain HTTP from
    this console, as ordinary DSi-mode homebrew with an idle ARM7 of its own. What that run
    could not tell us is whether the same driver comes up as a *guest of nds-bootstrap's
    ARM7*, which is a different core: it is not the libnds template, it does not open SCFG
    itself, it is already driving the SD/eMMC controller one instance below the WiFi SDIO
    block, and it owns the FIFO the launcher talks over. See docs/retroachievements.md,
    "#1f -- the rest of the ladder".

    Deliberately stops short of lwip. Everything up to and including the WPA2 handshake
    happens on the ARM7 inside dsiwifi; DHCP and sockets are the ARM9's lwip, and lwip as
    dsiwifi configures it does not fit the launcher's link region (see the note in
    docs/retroachievements.md). Splitting there is not a compromise: it is where the
    question step two asks actually ends, and the ARM9 half of this probe therefore links
    no library at all -- only dsiwifi's IPC header.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_WIFI_H
#define RA_WIFI_H

#include <nds/ndstypes.h>
#include <stddef.h>   /* size_t, for the ra_net entry points below */
/*
    RA_SYNTHETIC_ID_BASE, which the queue reader has to know: the cardengine side of this fork
    writes ids and the launcher side decides what to do with them, and "which ids are not real" is a
    fact both halves need and neither owns. Header-only, so this costs the ARM7 nothing.
*/
#include "ra.h"

/*
    Master switch, and it is off in every shipped build. Three values:

      **0** -- not built. The launcher is byte-for-byte what it was.

      **1** -- the diagnostic. Runs the whole ladder and *stops on a summary*, so a reading is a
      reading rather than something to catch before a game covers it. This is the mode every
      measurement in docs/retroachievements.md was taken in and it stays.

      **2** -- fetch, then boot. Same ladder, but it tears the radio down and returns, so the
      launcher goes on to start the game with the server's own achievement set already staged.

    Mode 1 halts for a second reason that mode 2 had to answer before it could exist. dsiwifi's
    bring-up blocks: wifi_card_wlan_init() spins in `while (1)` waiting for the firmware-ready
    flag and again in `while (!wmi_is_ready())`, with no timeout, inside a FIFO handler on the
    ARM7. If the chip does not come up that ARM7 is wedged -- and handing a wedged ARM7 to the
    bootloader, which is about to overwrite its code while a timer IRQ and an AUX IRQ are still
    live, is not something to do for a measurement.

    What makes mode 2 possible is that the teardown turned out to already exist. dsiwifi's
    DSiWifi_DisconnectAP() is an unimplemented sassert(false), but wifi_card_deinit() in its ARM7
    half does exactly the right four things: masks the SDIO card IRQ, disables the AUX IRQ,
    disables the ARM7's TIMER3, and clears the chip's own interrupt enables. The ARM9 half adds a
    TIMER3 of its own and a FIFO_DSWIFI handler, and both of those are ours to stop. See
    raWifiShutdown().

    So: `make RA_LAUNCHER_WIFI=1` measures, `=2` plays, and neither is a shipped build yet.
*/
#ifndef RA_LAUNCHER_WIFI
#define RA_LAUNCHER_WIFI 0
#endif

/* Mode 2 and above return to the launcher instead of halting on the summary. */
#define RA_WIFI_BOOTS_GAME (RA_LAUNCHER_WIFI >= 2)

/*
    Where the launcher writes what it saw. Root of the card, next to NDSBTSRP.LOG and
    alongside the probe's own /wifiprobe.log -- because the two are meant to be *diffed*,
    and that is the whole test design. Both capture dsiwifi's narration verbatim, so
    "did the chip arrive in the same state under our boot path" is a text comparison
    rather than a judgement call.
*/
#define RA_WIFI_LOG_PATH      "sd:/ra_wifi_launcher.log"
#define RA_WIFI_LOG_PATH_FAT  "fat:/ra_wifi_launcher.log"

/*
    A running list of every ROM this launcher has hashed, one line per game, never truncated.

    The log beside it is opened with "w" and so is overwritten on every boot, which is right for a
    diagnostic and wrong for this: finding a game whose set has an achievement worth testing means
    booting several ROMs and comparing them afterwards, and a file that only remembers the last one
    forces a copy off the card between each. This is append-only and deduplicated, so a card can be
    walked through a shelf of games and end up with the whole list.
*/
#define RA_HASHES_PATH        "sd:/ra_hashes.txt"
#define RA_HASHES_PATH_FAT    "fat:/ra_hashes.txt"
#define RA_HASHES_READ_MAX    8192   /* enough for ~200 entries; only used to avoid duplicates */


/*
    And where stage 12 writes the definitions it staged, verbatim, one per line.

    The first hardware run of that stage is why this exists. It reported 51 definitions in 6,791
    bytes and printed the first one as `1=1.300.` -- eight characters, which is *syntactically* a
    valid memaddr ("always true, 300 hits") and is not what a published achievement looks like.
    Six summary numbers cannot tell "the scanner works and that set has an odd first entry" apart
    from "the scanner is producing fragments", and no amount of arguing from the code settles it
    either.

    So the definitions become the artifact, the way dsiwifi's verbatim log is the artifact for the
    chip: this file can be read against the set's page on retroachievements.org, definition by
    definition. Same file format the launcher already *reads* from ra_achievements.txt, which
    makes it directly re-usable -- copy it there and the game boots with the server's own set.
*/
/*
    The per-game cache: the fetched set, kept so a later boot does not have to ask for it again.

    **Keyed by the ROM's hash, not by its GameID**, and that is the whole design. The GameID comes from
    r=gameid, which needs the network -- so a cache indexed by it could only ever be found by a boot
    that had already done the thing the cache exists to avoid. The hash is computed locally at stage 0b
    from the file on the card, with no radio up, and it identifies exactly one set.

    One file per ROM, so a card with a shelf of games has a set for each rather than for whichever one
    was copied by hand last. Without this the offline path only ever worked for the most recent game.
*/
#define RA_CACHE_DIR           "sd:/_nds/nds-bootstrap/ra"
#define RA_CACHE_DIR_FAT       "fat:/_nds/nds-bootstrap/ra"

#define RA_DEFS_DUMP_PATH      "sd:/ra_definitions.txt"
#define RA_DEFS_DUMP_PATH_FAT  "fat:/ra_definitions.txt"

/*
    The unlock queue: what the last play session earned, waiting to be sent.

    There is no way for the cardengine to reach the network. It runs inside the game, on the game's
    IRQ stack, with the radio torn down before the game ever started -- so an achievement earned
    while playing cannot be reported when it happens. It gets written here, and the *next* boot's
    launcher sends it. That is the whole shape of the loop, and it means an unlock is reported late
    rather than never.

    Two constraints decide the format, and they pull in opposite directions.

    The cardengine can only write into clusters that are already allocated -- that is how every other
    file nds-bootstrap writes from inside a game works (see fileWrite() and the srParams file). So
    the file is created by the launcher at a fixed size and never changes length: records are a fixed
    RA_QUEUE_RECORD bytes and record N lives at offset N*RA_QUEUE_RECORD, which is an offset the
    cardengine can compute without reading anything.

    And it has to be writable by hand, because that is what makes the sending half testable before
    the writing half exists: type an id into the file with any text editor, boot, and watch the
    server's answer. So the records are ASCII decimal and the parser treats every non-digit -- NUL
    padding included -- as a separator.

    Clearing is done in place, zeros over the same length, for the first reason: truncating the file
    could hand its clusters back and break the allocation the cardengine depends on.

    **A record carries when it was earned, which game it came from and which mode it was earned in,
    not only what.** The layout is

        <id>\t<YYYYMMDDhhmmss>\t<gamecode>\t<gametitle>\t<h>\n

    NUL-padded to RA_QUEUE_RECORD, and the stamp is local time straight off the console's RTC. It is
    there so `r=awardachievement` can send `o=` -- seconds since the unlock -- instead of letting the
    server date an achievement by when it was *submitted*, which with this client can be a day later.
    See raQueueStampToUnix() for the conversion and raQueueSign() for what `o=` does to the signature.

    The tab is the delimiter for the same reason it delimits the staged definitions block: it cannot
    occur inside any of these fields, so none of them needs escaping.

    **The game comes from the ROM's own header, not from the server.** `gameCode` and `gameTitle` are
    the first sixteen bytes of every DS cartridge header, and the cardengine already holds a pointer to
    the running game's. So the record can name its game at the moment the achievement fires, with no
    network and nothing passed down from the launcher -- which is the whole point, because the case
    that needs it is a queue full of game A's unlocks while game B is running and there is no WiFi.

    It also means RetroAchievements' own GameID is *not* here, and does not need to be. `a=` is a
    globally unique achievement id, so submission never needed the game; the only thing that needed it
    was being able to say which game an unlock came from, and the cartridge header says that better --
    offline, and as a name rather than a number.

    The cost is that `gameTitle` is the ROM's internal title: twelve characters, upper case, `CONTRA4`
    rather than `Contra 4`. Recognisable, free, and available with the radio down.

    **`<h>` is the mode the session was in when it fired, and it is here because nothing else lives
    long enough to hold it.** The mode used to be read from `ra.cfg` at submission time, which meant
    a player could earn unlocks in softcore with the in-game menu's RAM editor open, set
    `hardcore=1`, boot, and have them sent as hardcore. The session that earned an unlock is over by
    then -- the record is the only thing that crosses from it to the boot that sends it.

    `1` or `0`, written by the ARM7 from the mode that crossed in the request's own magic; see
    RA_SHARED_UNLOCK_HARDCORE. Absent reads as softcore, which covers a bare hand-typed id, a record
    from a build before this field, and a record whose clock failed so it has no fields at all. That
    is the fail-safe direction: the failure it allows is a genuine hardcore unlock sent as softcore,
    where the other direction is a claim the client cannot support.

    One consequence of appending rather than inserting, and it is worth stating because it is
    invisible until someone hits it: a **downgraded** build reads the mode digit as the next record's
    id. `0` is dropped as it always was, `1` would be sent once and answered `Unknown achievement`.
    Only reachable by going back to an older nds-bootstrap-ra after this one has written a queue.

    **Every field after the id is optional, and that is load-bearing.** A record that is a bare id --
    one a human typed, or one written by a build from before any of this existed -- parses exactly as
    it always did and is sent without `o=`. That is what keeps the file hand-writable and what makes
    each upgrade a non-event: an old queue drains normally, it just cannot say when or from where.

    The record has grown twice for this, 16 -> 32 for the stamp and 32 -> 48 for the game, and the file
    with it. It migrates itself: the launcher reads whatever length it finds and always writes
    RA_QUEUE_BYTES back, so the first boot on a new build extends the file in place.
*/
#define RA_QUEUE_PATH          "sd:/ra_unlocks.txt"
#define RA_QUEUE_PATH_FAT      "fat:/ra_unlocks.txt"
/*
    <id>\t<stamp>\t<code>\t<title>\t<h>\n is 10+1+14+1+4+1+12+1+1+1 = 46 at its longest. ASCII,
    NUL-padded. The record was already 48 when the mode field went in, so unlike the stamp and the
    game before it this one cost no migration -- every card in the field was already the right size.
*/
#define RA_QUEUE_RECORD        48
#define RA_QUEUE_STAMP         14                              /* YYYYMMDDhhmmss */
#define RA_QUEUE_CODE          4                               /* tNDSHeader.gameCode */
#define RA_QUEUE_TITLE         12                              /* tNDSHeader.gameTitle */
#define RA_QUEUE_MAX           64                              /* a session earning more is not a thing */
#define RA_QUEUE_BYTES         (RA_QUEUE_RECORD * RA_QUEUE_MAX)

/*
    The in-game viewer's index -- what the menu reads instead of parsing the definitions block.

    One entry per achievement, in the order the set arrived, with byte offsets into the block rather
    than pointers: the block sits at a different address in the launcher (where it is staged) and in
    DSi WRAM (where it is read), so a pointer recorded on one side is meaningless on the other.
    Offsets survive the copy the bootloader makes.

    Built by cardenginei_arm9_ra while the block is still pristine -- see
    CARDENGINEI_ARM9_RA_VIEWER_LOCATION for why the menu cannot do it later.

    An offset of 0 means the field is absent, which is unambiguous because offset 0 of the block is
    the first record's first byte and no field can start there. That is what a record degraded by a
    full block looks like from here: description gone, then points, then title.
*/
#define RA_VIEWER_MAGIC        0x31564152u   /* 'RAV1', and CARDENGINEI_ARM9_RA_VIEWER_MAGIC */
#define RA_VIEWER_MAX_ENTRIES  128

/* This achievement is already held by the account, so it is shown rather than armed. */
#define RA_VIEWER_EARNED       0x01
/*
    ...and this one is earned but not yet sent: it is in the queue file, or it fired this session.

    Distinct from EARNED because the two are different promises. EARNED is the server's answer and
    survives the card being lost; QUEUED is this console's word for it and does not.
*/
#define RA_VIEWER_QUEUED       0x02

/*
    When an achievement was earned, packed into 27 bits of a u32, and 0 means "not known".

        bits 0-5    minute   0-59
        bits 6-10   hour     0-23
        bits 11-15  day      1-31
        bits 16-19  month    1-12
        bits 20-26  year     0-127, from 2000

    **Packed rather than stored as text, and packed rather than left as a unix epoch**, for two
    different reasons. Text would cost sixteen bytes an entry against a viewer block whose whole
    reservation is a couple of kilobytes. An epoch would cost four, the same as this -- and would put
    a calendar conversion inside the in-game menu, which has no libc and no business owning one. The
    launcher has a real clock and a real library; it does the conversion once and the menu unpacks
    five fields with shifts.

    Zero cannot collide with a real value: month and day are 1-based, so a packed date always has a
    non-zero month field.
*/
#define RA_WHEN_MINUTE(w)  ((w) & 0x3F)
#define RA_WHEN_HOUR(w)    (((w) >> 6) & 0x1F)
#define RA_WHEN_DAY(w)     (((w) >> 11) & 0x1F)
#define RA_WHEN_MONTH(w)   (((w) >> 16) & 0x0F)
#define RA_WHEN_YEAR(w)    (2000 + (((w) >> 20) & 0x7F))

/*
    Build one. Returns 0 for anything out of range rather than a wrapped date, because a wrong date
    shown confidently is worse than no date at all -- the menu prints nothing for 0.
*/
u32 raWhenPack(int year, int month, int day, int hour, int minute);
/*
    ...and from the queue file's own stamp, `YYYYMMDDhhmmss`, which is where a locally earned unlock's
    time comes from. Returns 0 if the stamp is not fourteen digits.
*/
u32 raWhenFromStamp(const char* stamp);

typedef struct raViewerEntry {
	u32 id;
	u16 titleOff;
	u16 pointsOff;
	u16 descOff;
	u8  flags;
	u8  pad;
	/*
	    When it was earned, packed -- see RA_WHEN_MINUTE. Zero for an achievement this account does
	    not hold, and zero for one earned during *this* session: the flag flips while the game runs
	    and nothing in that context has a date, only the hours and minutes on sharedAddr.
	*/
	u32 when;
} raViewerEntry;

typedef struct raViewerBlock {
	u32           magic;        /* RA_VIEWER_MAGIC, so an unwritten window is not read as a set */
	u16           count;        /* entries below */
	u16           earned;       /* how many of them carry RA_VIEWER_EARNED */
	/*
	    ...and RA_VIEWER_QUEUED, which unlike the other two moves while the game runs: an achievement
	    that fires this session is queued from that moment. Kept beside the flag rather than counted
	    by walking the entries at draw time, because the menu should render what it is given.
	*/
	u16           queued;
	u16           pad;
	raViewerEntry entry[RA_VIEWER_MAX_ENTRIES];
} raViewerBlock;

#define RA_PENDING_MAGIC       0x31504152u   /* 'RAP1' */
#define RA_PENDING_GAMES_MAX   8
/* Queued ids the viewer can mark; see raPendingBlock::queued. */
#define RA_PENDING_QUEUED_MAX  32

typedef struct raPendingGame {
	char code[RA_QUEUE_CODE + 1];    /* the ROM's gameCode, NUL-terminated */
	char title[RA_QUEUE_TITLE + 1];  /* its gameTitle, trimmed */
	u16  count;                      /* unlocks of this game still owed */
	u16  waitDays;                   /* how long the oldest of them has waited */
} raPendingGame;

typedef struct raPendingBlock {
	u32 magic;                       /* RA_PENDING_MAGIC, so an uninitialised block is not read */
	u16 games;                       /* entries below that are filled */
	u16 total;                       /* unlocks across all of them, including any dropped games */
	u16 dropped;                     /* games past RA_PENDING_GAMES_MAX, so the menu can say so */
	u16 unnamed;                     /* records with no game -- hand-typed, or from an older build */
	/*
	    The game this boot is about to start, from its own header, and how many unlocks it has earned
	    since. `session` is written by cardenginei_arm9_ra as it hands ids to the ARM7 -- the block is
	    in the window that binary owns, so it is a store rather than a message.

	    Without it the page would show what was owed *at boot* and nothing since, so earning an
	    achievement and opening the menu to check would show it missing until the next boot -- which is
	    the one question the page most obviously has to answer.
	*/
	char thisCode[RA_QUEUE_CODE + 1];
	char thisTitle[RA_QUEUE_TITLE + 1];
	u16  session;
	raPendingGame game[RA_PENDING_GAMES_MAX];
	/*
	    The ids sitting in the queue file at boot, so the viewer can mark them.

	    The counts above answer "how much is owed"; this answers "is *this* achievement one of them",
	    which is the question the achievements page asks and the only one a per-game tally cannot.

	    Here rather than in the viewer's own index because of who knows what and when. The launcher
	    reads the queue file and cardenginei_arm9_ra builds that index, and the two never meet -- but
	    this block already crosses between them. cardenginei_arm9_ra matches these against the set as
	    it indexes it.

	    Bounded well under RA_QUEUE_MAX on purpose. A queue deeper than this is a card that has been
	    offline for a long time, and the honest failure there is a few unmarked rows rather than 256
	    bytes taken off a 512-byte reservation to cover a case nobody is in.
	*/
	u16  queuedCount;
	u32  queued[RA_PENDING_QUEUED_MAX];
	/*
	    ...and when each of them was earned, packed, from the queue record's own stamp. Parallel to
	    the array above rather than folded into it because that one is matched against by id, and an
	    id is what the cardengine has.

	    This is the *local* clock's answer, which the server's is converted into as well -- see
	    raWifiEarnedWhen(). Two different sources for the same kind of fact have to agree about what
	    kind of fact it is, or the page shows one achievement in local time and the next in UTC.
	*/
	u32  queuedWhen[RA_PENDING_QUEUED_MAX];
} raPendingBlock;

/*
    The size below which nds-bootstrap does not install the cheat engine, and therefore the line
    between "a cheat file is present" and "cheats are on".

    It is 4 because an empty cheat list is not an empty file: TWiLight Menu writes cheatData.bin when
    the player opens the cheat screen for a game, and turning every cheat back off leaves the file
    there holding only its terminator. `cheatSizeTotal > 4` is what the bootloader tests -- see
    `cheatsEnabled` in main.arm7.c and hook_arm7.c -- and it is restated here because the launcher
    has to answer the same question about the same card, hours earlier in the boot, to decide whether
    a session may claim hardcore.

    Only the floor is shared. Each of those sites also refuses a total too large for the room it has,
    and that ceiling is computed from the ROM's layout: nothing outside the bootloader can know it.
*/
#define RA_CHEATS_MIN_BYTES    4

#define RA_SESSION_MAGIC       0x31534152u   /* 'RAS1' */

/*
    What the in-game menu needs to know about this boot's session, staged before the radio is
    touched. See CARDENGINEI_ARM9_RA_SESSION_LOCATION for why it is not part of the block above.

    One flag so far, and it is a capability rather than a preference: `hardcore` set means the menu
    must refuse to *write* memory, because everything this session queues will be claimed as
    hardcore when some later boot has a network to claim it on. The menu reads memory either way --
    RetroAchievements' rules are about editing, and a hex dump of a running game is a debugging
    tool this fork has no reason to take away.

    Written by the launcher, read by cardenginei_arm9_igm, and by nothing else. The cardengine that
    evaluates achievements does not consult it: it queues what it earns, and whether that queue may
    be sent as hardcore was decided here, at boot, by the same word.
*/
/*
    Why hardcore was not granted, for the menu to say so. Only meaningful when the player asked for
    it: `RA_REFUSED_NONE` with `hardcore` clear means ra.cfg said softcore, or there is no ra.cfg.

    A reason code rather than a string because the block crosses into a binary with no formatter and
    the set of reasons is small and closed. It is open at the end on purpose -- the User-Agent is the
    refusal this project expects to add next, and it will cost a number and a line in the menu.
*/
#define RA_REFUSED_NONE        0
#define RA_REFUSED_CHEATS      1   /* the cheat engine runs this session */

typedef struct raSessionBlock {
	u32 magic;      /* RA_SESSION_MAGIC, so an unwritten window is not read as an answer */
	/*
	    The mode the launcher settled on, and the byte the bootloader may clear if it finds it is
	    installing the cheat engine after all. **A plain flag, and it stays one**: the RAM viewer and
	    the unlock path both read it as a capability, and a field that grew values would have those
	    two testing `!= 0` against an enum.
	*/
	u8  hardcore;
	u8  refusal;    /* RA_REFUSED_*, for display only -- nothing gates on it */
	/*
	    Whether the on-screen notification may draw at all, from `overlay` in ra.cfg. Defaults on.

	    It exists because the overlay is the one part of this fork that **negotiates with a running
	    game for its hardware**, and negotiation can be got wrong -- it has been, four times, on one
	    shooter. Everything else here either reads memory or writes to a window nobody else owns.

	    So this is the switch that makes the overlay's risk optional rather than structural: with it
	    off, an unlock is still detected, still queued, still submitted and still listed in the menu
	    with its date. What is lost is the popup, which is the only part that can break a game.

	    Rides here rather than in a block of its own because this block already crosses from the
	    launcher to the cardengine and had two spare bytes.
	*/
	u8  overlay;
	/*
	    Whether an unlock may be written to sd:/ra_unlocks.txt, from `queue` in ra.cfg.

	    Gated on the ARM9 side rather than the ARM7's, and that is forced rather than chosen: this
	    block lives in DSi WRAM, which is the ARM9's, so the CPU that does the writing cannot read the
	    switch. ra_rc_offer_unlock() simply does not hand the id over, which stops the whole chain --
	    no FIFO request, no file read, no sector write -- one step earlier than the thing being tested.
	*/
	u8  queue;
} raSessionBlock;

/*
    What one pass over the queue did, so the log can say it rather than imply it.

    `kept` is the number that has to survive into the next boot, and it is the field that makes this
    a queue rather than a fire-and-forget: an id whose request never reached the server is still
    owed. See raWifiSubmit() for the rule that decides between kept and cleared.
*/
typedef struct raQueue {
	u32  ids[RA_QUEUE_MAX];  /* what the file held, deduped, in file order */
	/*
	    When each of those was earned, as Unix seconds, or 0 for "the record did not say" -- a bare id
	    from a hand-edited file or from a build before the stamp existed. Parallel to `ids` rather than
	    a struct, because `ids` is passed on its own to the sender and this is only ever read beside it.
	*/
	u32  times[RA_QUEUE_MAX];
	/*
	    ...and the same instant packed for the in-game menu -- see RA_WHEN_MINUTE. Taken from the
	    stamp string directly rather than converted back from `times`, which is what keeps this file
	    free of calendar arithmetic: the stamp is already `YYYYMMDDhhmmss` and the packing is five
	    substrings, where an epoch would need a real date library the menu side does not have.
	*/
	u32  packed[RA_QUEUE_MAX];
	/*
	    Which game each came from, straight out of that ROM's header and NUL-terminated here. Empty
	    when the record did not say. `codes` is what identifies a game -- it is unique per release --
	    and `titles` is what a human reads.
	*/
	char codes[RA_QUEUE_MAX][RA_QUEUE_CODE + 1];
	char titles[RA_QUEUE_MAX][RA_QUEUE_TITLE + 1];
	/*
	    Which mode each was earned in: 1 for hardcore, 0 for softcore or for a record that did not
	    say. This is what `h=` is taken from when the unlock is submitted, **not** ra.cfg -- see
	    RA_QUEUE_PATH for why the record has to be the authority.

	    A u8 rather than sharing a bit with something: 64 bytes against a structure already holding
	    64 titles, and a flag packed into `ids` would be a flag inside a number this file is careful
	    to keep meaning exactly one thing.
	*/
	u8   hardcore[RA_QUEUE_MAX];
	int  stamped;            /* how many of those carried a usable stamp */
	int  hard;               /* how many of those were earned in hardcore */
	int  named;              /* how many of those named their game */
	int  count;              /* how many of those */
	int  dropped;            /* unparseable or out of range, so the file said something we ignored */
	/*
	    Records carrying a synthetic id -- see RA_SYNTHETIC_ID_BASE. Never sent, never counted as
	    owed, and counted here so a card cleaning itself of them says so once instead of going quiet.

	    Separate from `dropped` because these are not malformed. They are perfectly well-formed
	    records that a build before the ARM9 guard wrote from its own self-test, and telling the
	    two apart is what says whether a card is being cleaned or a file is corrupt.
	*/
	int  synthetic;
	int  truncated;          /* the file held more than RA_QUEUE_MAX */
	int  sent;               /* the server answered, whatever it answered */
	int  accepted;           /* ...and said Success */
	int  refused;            /* ...and said otherwise; the body is logged */
	int  kept;               /* never got an answer, so still owed */
} raQueue;

/*
    The ladder, and it only moves forward. Five rungs against the probe's six: rungs 1-3
    are the chip coming up, 4-5 are the link becoming usable, and the probe's stages 3-6
    (DNS, TCP, HTTP) are step three of the plan rather than this one.

    Each rung is named after the thing that proves it, so a stopped run says where it
    stopped instead of that it failed.
*/
#define RA_WIFI_STAGE_START       0  /* the ARM7 never said anything */
#define RA_WIFI_STAGE_CHIP        1  /* SDIO answered: the manufacturer/chip-ID read worked */
#define RA_WIFI_STAGE_FIRMWARE    2  /* the Xtensa core is running our firmware */
#define RA_WIFI_STAGE_WMI         3  /* WMI is up: the driver has a working command channel */
#define RA_WIFI_STAGE_ASSOCIATED  4  /* WMI_CONNECT_EVENT -- associated to the AP */
#define RA_WIFI_STAGE_READY       5  /* handshake done, keys installed, link usable */
#define RA_WIFI_STAGE_IP          6  /* DHCP gave us an address */
#define RA_WIFI_STAGE_RESOLVED    7  /* DNS answered for retroachievements.org */
#define RA_WIFI_STAGE_CONNECTED   8  /* TCP to port 80 */
#define RA_WIFI_STAGE_ANSWERED    9  /* the API replied, and the reply looks like the API */
#define RA_WIFI_STAGE_LOGGED_IN   10 /* r=login returned a token for the configured user */
#define RA_WIFI_STAGE_IDENTIFIED  11 /* r=gameid turned the ROM's hash into a GameID */
/*
    r=awardachievement for whatever the last play session earned, and it comes *first* of the three
    game-specific rungs for a reason that is not politeness.

    The cardengine cannot reach the network, so an unlock earned while playing is written to a queue
    file and sent on the next boot. If that send happened after the fetch, the achievement just
    awarded would still be in the block, would trigger again next session, and would be queued again
    -- forever. Sending before r=unlocks means the server already knows about it when we ask what the
    account holds, so the very next rung filters it out of the block. The ordering is what makes the
    loop close instead of spin.
*/
#define RA_WIFI_STAGE_SUBMIT      13
/*
    r=startsession, which this client did not send at all until an award came back Success:true and the
    achievement did not appear on the account.

    rcheevos sends it when a game loads, before anything else game-specific, and it is the verb that
    tells the server a play session exists. Whether RA requires one before it will record an unlock is
    the open question it was added to answer -- but it is part of a correct client either way, so it is
    not a guess being built on spec.

    Its reply is independently useful: `Unlocks` and `HardcoreUnlocks`, as arrays of objects, which is a
    second and differently-shaped source for the same thing `r=unlocks` reports. Two sources that can be
    compared is exactly what the current question needs.
*/
#define RA_WIFI_STAGE_SESSION     12
/*
    r=unlocks, and it sits *before* the patch because that is the only order in which it is useful:
    knowing which achievements the account already has is what lets the scanner leave them out of a
    block that is 88% full.
*/
#define RA_WIFI_STAGE_UNLOCKS     14
#define RA_WIFI_STAGE_PATCHED     15 /* r=patch put real definitions in the staging block */
#define RA_WIFI_STAGE_MAX         RA_WIFI_STAGE_PATCHED

/*
    What dsiwifi's narration said about how the chip arrived.

    Read out of its log text rather than from the driver, because none of it is exposed:
    the chip string, the host-interest flag and the BMI version are statics inside
    libs/dsiwifi's ARM7 half. That is a coupling to a third-party library's printf strings
    and it is worth saying so plainly -- which is exactly why the verbatim log is the
    primary artifact and this struct is the convenience. The host test pins every string
    against the log a real console produced, so a submodule bump that renames one fails
    tools/ra_reader_test.sh instead of quietly reporting the wrong world.
*/
typedef struct raWifiVerdict {
	u8   chipSeen;         /* "Mfg ..." -- the chip answered on the SDIO bus */
	u8   coldStart;        /* "needs firmware upload" -- host-interest flag read 0 */
	u8   bmiSeen;          /* "BMI version:" -- the bootloader answered */
	u8   firmwareLaunched; /* "Launching!" */
	u8   firmwareReady;    /* "ready, handshaking..." */
	u8   wmiReady;         /* "fully initialized!" */
	u8   associated;       /* "WMI_CONNECT_EVENT" */
	u8   linkReady;        /* "Done auth" -- the handshake finished */
	u8   mboxAllocFailed;  /* "bad mbox alloc" -- the ARM7 heap could not spare 3K */
	u8   reserved[3];

	/*
	    Set by the probe rather than read out of the log: from step 3 on, the top of the
	    ladder is lwip's business and lwip answers in return values, not in narration.
	*/
	u8   gotIp;
	u8   dnsOk;
	u8   tcpOk;
	u8   apiOk;
	u8   loggedIn;         /* r=login returned a token */
	u8   identified;       /* r=gameid returned a non-zero GameID */
	u8   sessionOk;        /* r=startsession answered Success */
	u16  sessionUnlocks;   /* ...with this many softcore unlocks for this game */
	u16  sessionHardcore;  /* ...and this many hardcore ones */
	/*
	    The submit rung completed -- which includes having nothing to submit. An empty queue is a
	    successful pass over it, and letting it read as a failure would cap the ladder at 11 on every
	    boot that did not earn anything, which is most of them.
	*/
	u8   submitDone;
	u16  submitAccepted;   /* ids the server took */
	u16  submitRefused;    /* ids it answered about and did not take */
	u16  submitKept;       /* ids still owed, carried to the next boot */
	u8   unlocksKnown;     /* r=unlocks answered, so the skip list is trustworthy */
	u16  unlockCount;      /* how many the account already has */
	u8   patched;          /* r=patch produced at least one definition */
	u16  defsKept;         /* ...this many */
	u32  defsBytes;        /* and this much of the staging block */
	u32  gameId;           /* ...this one. Zero means the server does not know the dump */
	u32  ip;               /* as DSiWifi_GetIP() returns it */
	char chip[16];         /* "AR6014", as dsiwifi named it */
	/*
	    The partial line being accumulated -- not a result. Larger than dsiwifi's own 0x7C
	    print buffer on purpose: several of its calls print without a trailing newline, so
	    fragments concatenate here until one arrives. Concatenation cannot hide a match;
	    a forced flush at the end of this buffer could, by cutting a string in two, so the
	    buffer is sized so that never happens for anything the bring-up prints.
	*/
	char line[256];
	u16  lineLength;
	u16  lines;            /* how many complete lines were classified */
} raWifiVerdict;

#ifdef __cplusplus
extern "C" {
#endif

/*
    The classifier. Pure text in, flags out, no I/O -- so it is the one part of this that a
    host can check, and tools/ra_reader_test.c does.

    Fed a line at a time on purpose. dsiwifi's ARM7 ships its log over the FIFO in 59-byte
    chunks, so a single line can arrive split across two messages; matching per chunk would
    miss any string that straddles the boundary, and the strings that matter are long ones.
    raWifiVerdictChunk() reassembles, raWifiVerdictLine() is what does the deciding.
*/
void        raWifiVerdictReset(raWifiVerdict* v);
void        raWifiVerdictLine(raWifiVerdict* v, const char* line);
void        raWifiVerdictChunk(raWifiVerdict* v, const char* chunk);
void        raWifiVerdictFlush(raWifiVerdict* v);
int         raWifiVerdictStage(const raWifiVerdict* v);
const char* raWifiVerdictArrival(const raWifiVerdict* v);

/*
    Step 3c's configuration, read from sd:/_nds/nds-bootstrap/ra.cfg -- the same `key=value`
    shape odelot's MiSTer core uses, because that is the file this project's user already has.

    The password is kept in the file, knowingly: the alternative of caching only a token was
    offered and declined. It therefore goes over the wire in the clear on every boot. It must
    never reach the log -- see raConfigRedact().
*/
#define RA_CFG_PATH      "sd:/_nds/nds-bootstrap/ra.cfg"
#define RA_CFG_PATH_FAT  "fat:/_nds/nds-bootstrap/ra.cfg"

/*
    **Whether the on-screen popup may draw at all, and the default is now "no".**

    This inverts what this fork did for its whole life, and it is a decision taken on evidence rather
    than on taste. The overlay is the one part of this client that negotiates with a running game for
    its hardware: it works out which background layer, character block, sprite slot or object VRAM
    the game is not using, and borrows it. That judgement is an *inference* over registers the game
    rewrites every frame, and it has now been wrong on hardware in six separate rounds.

    What settled it is a reading rather than an argument. On Ketsui the nine VRAM banks come back:

        A B  3D textures    C  sub BG    D  sub OBJ    E  texture palette
        F G  LCDC           H I  disabled

    The two free banks are the two that can only be mapped where C and D already are -- bank I's only
    sub-engine homes are 0x06208000 and 0x06600000, both inside banks C and D. And with all four of
    A-D spoken for, the main engine's capture route is out as well. **On that game there is nowhere
    to draw that can be shown to be safe.** No amount of further surveying changes that, because the
    premise the design rests on -- that a free corner can always be found -- is simply false there.

    So the trade is inverted. A notification that does not appear is a missing feature; a game whose
    graphics are corrupted is a bug, and it is the worse of the two. The popup now draws only where a
    person has watched it and said it was fine:

      0  never
      1  only for games listed in `overlay_games`   <- the default
      2  always, on any game

    Level 2 is the old behaviour, kept and reachable, because a player who wants it on an unlisted
    game should be able to choose that for themselves. It is opting into a known risk rather than
    inheriting one.
*/
#define RA_OVERLAY_OFF    0
#define RA_OVERLAY_LISTED 1
#define RA_OVERLAY_ALWAYS 2
#define RA_OVERLAY_MAX    2

/*
    Resolved in the launcher, not the cardengine, and staged as the single bit raSessionBlock.overlay
    already carries -- so the game-side binaries do not change at all and the tight ones spend nothing.
    The launcher is the side that knows the GameID and has the room to parse a list.

    `gameId` is the server's, and zero -- a ROM RetroAchievements does not recognise -- is never on a
    list, so it never draws at RA_OVERLAY_LISTED.

    Pure, and tested in tools/ra_launcher_test.c: matching has to be whole-number, or `1671` in the
    list would enable `16710`.
*/
int raOverlayAllowed(u8 level, const char* list, u32 gameId);

typedef struct raConfig {
	char username[33];   /* RA usernames are short; 32 is well past any real one */
	char password[65];
	u8   hardcore;       /* the API's h= parameter. Softcore is h=0 and is what this fork does */
	/*
	    `submit=0` in ra.cfg: read the queue, report it, send nothing and clear nothing. For testing --
	    an unlock is spent once it lands, because the server then returns it in r=unlocks and the
	    scanner leaves it out of the block. Defaults to 1.
	*/
	u8   submit;
	/*
	    `sync=0` in ra.cfg: skip the network ladder entirely and play from the per-game cache. Measured
	    reason -- a boot with no AP in range burns forty seconds failing to associate, and one with an AP
	    spends fifteen re-fetching yesterday's set. Detection, notification and queueing all keep working
	    without it, because none of them ever touched the radio. Defaults to 1.
	*/
	u8   sync;
	u8   debug;
	/*
	    `verbose_log=1` in ra.cfg: put the whole ladder on the screen, as it was before this key
	    existed. Defaults to **0**, which shows a progress bar, the step being worked on in ordinary
	    words, and a handful of essential lines at the end.

	    It governs **the screen only. The log file always gets everything.** That asymmetry is the
	    point of the key rather than a shortcut: the file is the channel this project's hardware
	    findings have all arrived through, and a setting that silenced it would mean a reflash before
	    any problem could be looked at. What was too verbose was never the file -- it was reading
	    fifty lines of chip bring-up on a screen while waiting to play.
	*/
	u8   verboseLog;
	/*
	    `overlay=0` in ra.cfg: detect, queue, submit and list unlocks, and never draw the popup. The
	    overlay is the only part of this fork that negotiates with a running game for its hardware --
	    see RA_CFG_OVERLAY. Defaults to 1.
	*/
	u8   overlay;
	/*
	    Which games the popup is allowed on, by RetroAchievements GameID, comma separated.
	    `overlay_games=16710,3405` and so on. Only consulted at RA_OVERLAY_LISTED.

	    **By GameID rather than by the ROM's four-character code**, and that is the canonical choice
	    rather than the convenient one: the GameID is what RetroAchievements itself keys on, it is
	    already printed in the launcher log for every boot, and it is the number in the URL of the
	    game's page. A four-character code is per-region and per-revision; the same game would need
	    several entries and a player would have to find them.
	*/
	char overlayGames[64];
	/*
	    `queue=0` in ra.cfg: detect and show unlocks, and never write one to the card. A diagnostic
	    for a game that freezes on the frame an achievement fires -- see RA_CFG_QUEUE. Defaults to 1,
	    because with it off an unlock does not survive the session.
	*/
	u8   queue;
	u8   found;          /* the file existed */
	u8   usable;         /* ...and both credentials are set */
	u16  notYet;         /* keys we recognise from odelot's file and do not act on yet */
	u16  unknown;        /* keys nobody knows: almost certainly a typo */
	u16  badLines;       /* non-comment lines with no `=` */
} raConfig;

/*
    Step 3c's transport. The layering table has had an `ra_net` row since before it existed;
    this is it. Negative returns so a failure names its own step instead of becoming a zero.
*/
/*
    The quiet screen's steps, in the order they happen, and the count is the bar's denominator.

    Named for what a player is waiting for rather than for the API call underneath -- "Signing in"
    rather than "stage 10: r=login" -- because the audience for this screen is someone who wants to
    know whether to keep waiting.
*/
#define RA_STEP_GAME      1   /* reading the ROM and hashing it */
#define RA_STEP_SETTINGS  2   /* ra.cfg */
#define RA_STEP_WIFI      3   /* chip up, associated */
#define RA_STEP_ADDRESS   4   /* DHCP */
#define RA_STEP_SIGNIN    5   /* r=login */
#define RA_STEP_LOOKUP    6   /* r=gameid */
#define RA_STEP_SESSION   7   /* r=startsession */
#define RA_STEP_SENDING   8   /* r=awardachievement for the queue */
#define RA_STEP_EARNED    9   /* r=unlocks */
#define RA_STEP_DOWNLOAD  10  /* r=patch */
#define RA_STEP_MAX       10

/*
    Render `step` of `steps` as a bar and a percentage, into at least RA_BAR_MIN bytes.

    Pure, and its own function, for the reason ra_rc_frame_skip() is one: it is the part with the
    arithmetic and tools/ra_launcher_test.c drives it directly. Getting a percentage wrong on a
    progress bar is the kind of bug that is invisible until someone counts.
*/
#define RA_BAR_CELLS 22
#define RA_BAR_MIN   32
void raWifiBar(char* out, u32 size, u8 step, u8 steps);
/*
    One cell of "still working", pulsing rather than spinning. See the note on the definition.
*/
char raWifiSpinFrame(u8 tick);
/*
    The column a `cells`-wide string starts at to sit centred in a `width`-wide console, never close
    enough to the edge for the console to wrap. See the note on the definition.
*/
u32 raWifiCentre(u32 width, u32 cells);

#define RA_NET_HOST           "retroachievements.org"

/*
    Who this client says it is.

    In one place now, and that is not tidiness either: the User-Agent was written out twice in ra_net.c,
    and it has become a *suspect*. RetroAchievements identifies clients by it, does not recognise this
    one, and answers by injecting a `Warning: Unknown Emulator` achievement into every set it serves us
    and blocking hardcore. Whether it also declines to record softcore unlocks is an open question -- see
    docs/retroachievements.md -- and a value under investigation should not exist in two copies.

    The honest name, not a known emulator's. Getting recognised is a request to make to
    RetroAchievements, not a string to borrow; a client that lies about what it is would be both against
    their rules and useless as evidence about this exact question.

    RA_NET_CLIENT_VERSION is also `l=` in r=startsession, which is the client library version. rcheevos
    sends its own there; this sends this project's, for the same reason.
*/
#define RA_NET_CLIENT_NAME    "nds-bootstrap-ra"
#define RA_NET_CLIENT_VERSION "0.1"
#define RA_NET_USER_AGENT     RA_NET_CLIENT_NAME "/" RA_NET_CLIENT_VERSION
#define RA_NET_PORT           80
#define RA_NET_RECV_TIMEOUT   5

#define RA_NET_BAD_ARGS       (-1)
#define RA_NET_NO_DNS         (-2)
#define RA_NET_NO_SOCKET      (-3)
#define RA_NET_NO_CONNECT     (-4)
#define RA_NET_REQ_TOO_LONG   (-5)
#define RA_NET_NO_SEND        (-6)

/*
    How many times to try the socket-and-connect before giving up, and why there is a number here at
    all.

    A run of the full ladder opened its third socket and lwip fired
    `LWIP_ASSERT("state!", msg->conn->state != NETCONN_CONNECT)` from api_msg.c:1411 -- the assert
    *after* the semaphore wait, which only trips when `sys_arch_sem_wait()` returns without the
    connect having completed. Netconns come from a static pool of eight, so a recycled netconn whose
    `op_completed` semaphore was left signalled by an earlier operation produces exactly that.

    This is not fixed here and the honest reason is that it is a race inside a vendored lwip, on a
    console with no debugger, and the tooling to chase it does not exist in this project. What a retry
    does is turn a random abort of the whole fetch into a logged hiccup -- a second attempt draws a
    different netconn from the pool. `attempts` is reported so the frequency becomes data rather than
    an impression.
*/
#define RA_NET_CONNECT_TRIES 3
#define RA_NET_RETRY_FRAMES  15   /* about 250 ms, so lwip's own timer thread gets to run */

/* How far one request got, so the ladder can be filled in from a single call. */
typedef struct raNetProgress {
	u8  resolved;
	u8  connected;
	u8  sent;
	u8  closedByPeer;
	u8  attempts;      /* socket-and-connect tries used; more than 1 means the race above */
	u32 address;
} raNetProgress;

/*
    Step 3d's reader. `r=login` and `r=gameid` fit in a 2 K buffer; `r=patch` returns a whole
    achievement set, which for a big game is over 100 K -- more than the launcher's remaining
    heap and more than three times the staging block it is destined for. So it is never held:
    the bytes are handed to a sink as they arrive off the socket, and what survives the call is
    the definitions, not the reply.

    Two things sit between the socket and the sink, and both are why this is a struct rather
    than a callback:

      The HTTP headers have to be skipped, and their length is not known in advance -- so the
      boundary can fall anywhere, including between the `\r\n` and the `\r\n`.

      A 1.1 reply may be **chunked**, and a chunk header is a hex length written *into the byte
      stream*. The small replies this project has already read came back unchunked, which
      proves nothing about a 100 K one -- and a `1a2f\r\n` landing in the middle of a memaddr
      string would corrupt exactly one definition, silently. So the framing is undone here,
      where it can be tested on a host, rather than hoped about.
*/
typedef void (*raNetSink)(void* ctx, const char* data, int length);

#define RA_NET_LINE_MAX 128

#define RA_NET_STREAM_STATUS  0  /* the response line */
#define RA_NET_STREAM_HEADER  1  /* header lines, until a blank one */
#define RA_NET_STREAM_BODY    2  /* identity encoding: everything left is body */
#define RA_NET_STREAM_SIZE    3  /* chunked: reading a hex chunk length */
#define RA_NET_STREAM_DATA    4  /* chunked: inside a chunk */
#define RA_NET_STREAM_CRLF    5  /* chunked: the CRLF that follows a chunk */
#define RA_NET_STREAM_TRAILER 6  /* the last chunk was empty; nothing more is body */

typedef struct raNetStream {
	raNetSink sink;
	void*     ctx;
	u8        state;
	u8        chunked;
	u16       status;      /* the HTTP status line's code, 0 if it did not parse */
	u32       chunkLeft;
	u32       bodyBytes;   /* how much reached the sink -- the body's real length */
	/*
	    What the reply said its body would be, and whether the whole of it has arrived.

	    **This is where fifty of the sixty seconds were.** Reading until the peer closes means
	    waiting for the peer to close, and the timing line put every one-kilobyte request at 10.2
	    seconds against a chip and a DHCP lease that together cost 5. The bytes were never the cost;
	    the wait after them was.

	    An earlier note here said Content-Length was not needed because `Connection: close` and the
	    chunk terminator both say where the body ends. Both do -- and one of them says it by making
	    the client wait for a socket to shut down. Enforced rather than believed: `done` is set only
	    when the counted bytes reach the declared length, so a truncated reply still ends at the
	    close it always ended at.
	*/
	u32       contentLength;   /* 0 = the reply did not say */
	u8        done;            /* the body is complete: stop reading */
	u16       lineLength;
	char      line[RA_NET_LINE_MAX];
} raNetStream;

/*
    Step 3d's scanner: pull every `"MemAddr":"..."` out of an `r=patch` reply, as it arrives,
    straight into the staging block the cardengine reads.

    Not a JSON parser, and the reason is not laziness -- it is that a parser for this reply
    needs the reply, and the reply does not fit. What makes the shortcut sound rather than
    lucky is that JSON escapes any quote inside a string as `\"`, so the eleven bytes
    `"MemAddr":"` cannot occur *inside* a value: an achievement titled `"MemAddr":"` arrives as
    `\"MemAddr\":\"` and does not match. The needle can therefore only be a key.

    What it does not do is know which object the key was in. Today `MemAddr` appears only on
    achievements -- leaderboards use `Mem`, rich presence `RichPresencePatch` -- so this takes
    the achievement set and nothing else. If RA ever adds a `MemAddr` elsewhere, this would
    take that too, and that is a limitation rather than a bug to find later.
*/
/*
    8192, and the number is a hardware measurement rather than a guess.

    It was 2048, chosen because RA memaddr strings "run from tens to a few hundred characters".
    That is true of most of them and false where it matters: the first run of stage 12 against
    GameID 14856 reported `longest memaddr 6264 of 2047` and **dropped five definitions** for
    exceeding the buffer. A completionist achievement is one condition per collectable -- 150
    stars is 150 conditions -- so a few kilobytes is normal for exactly the achievements a player
    cares most about.

    Raising it is cheap in the launcher (6 KB of .bss against 69,632 of measured headroom) and it
    is the only honest option: the alternative was already rejected in ra_patch.c, because a
    truncated memaddr is not a shorter achievement but a different one.

    8192 leaves 30% over the largest definition this project has seen. `longest` is reported on
    every run precisely so that margin stays a measurement -- another game will have another
    largest, and the log will say so instead of five achievements quietly not existing.
*/
/*
    Ids at or above this are not published achievements, on the evidence of one set and two lookups:
    the set for GameID 14856 staged **56** core definitions while retroachievements.org lists **55**,
    and the extra one -- `101000001:1=1.300.` -- returns NOT FOUND at
    retroachievements.org/achievement/101000001. Every genuine id in that set is between 92,869 and
    579,308.

    It is **not filtered on that basis**, and the distinction matters. One set and one id is not a
    rule, and a filter inferred from it would silently drop something real the first time RA numbers
    an achievement differently. What happens instead is that the reply's own bytes around such an id
    are captured and logged, so the next run says what the object *is* -- its Title, its Points, its
    Type -- rather than leaving this project with a threshold it cannot justify.

    The likely explanation is already written down as a known limitation: the scanner does not know
    which object a `MemAddr` belonged to, and this game has subsets -- its page redirects to
    `game/9983?set=6112`. A flat scan over a reply with more than one set reads across all of them.
*/
#define RA_ODD_ID_FROM 100000000u

/* Enough of the reply after an odd id to show its Title, Points, Flags and Type. */
#define RA_ODD_CONTEXT_MAX 240

#define RA_PATCH_MEMADDR_MAX 8192

/*
    Bytes kept of an achievement's Title, terminator included -- so 31 characters.

    Sized from the display rather than from what RA sends, which is the honest place to take it from:
    the notification is one row of a 32-tile background with a tile of margin each side, so 30
    characters is what can be shown and a 31st is already generous. Storing more would cost block
    space -- the scarcest thing in this project at 88% full on a real set -- to hold text nothing
    could ever draw.

    Longer titles are truncated and counted in `titleCut`, so the log says it happened rather than
    leaving a clipped label to be discovered on a photograph.
*/
#define RA_PATCH_TITLE_MAX 32

/*
    Bytes kept of an achievement's Description, terminator included -- so 64 characters.

    Sized from the viewer rather than from what RA sends: the in-game menu is 32 columns, so 64
    characters is two full lines under the title, which is as much as a page can give one entry
    while still listing others. RA's own descriptions run from about 40 to 120 characters, so this
    keeps most of them whole and clips the long ones.

    It is the most expensive field in the record and the first one dropped when the block runs out.
    See the degradation order below.
*/
#define RA_PATCH_DESC_MAX 65

#define RA_PATCH_SCAN   0
#define RA_PATCH_VALUE  1
#define RA_PATCH_FLAGS  2
#define RA_PATCH_ID     3
#define RA_PATCH_TITLE  4
#define RA_PATCH_DESC   5
#define RA_PATCH_POINTS 6
#define RA_PATCH_RICH   7

/*
    Each staged line is `<id>:<memaddr>`, and the id is the achievement's own number on
    retroachievements.org.

    Without it nothing downstream is possible: `r=unlocks` answers in ids, `r=awardachievement` is
    asked in ids, and "which achievement just fired" has no answer the server would recognise. The
    block carried none until now because nothing needed them, and adding them while the format has
    exactly one producer and one consumer is far cheaper than adding them later.

    **A leading run of decimal digits followed by a colon is an id, and nothing else is.** That is a
    property of memaddr syntax rather than a convention chosen here: every prefix flag that ends in a
    colon -- `A:`, `M:`, `N:`, `O:`, `P:`, `Q:`, `R:`, `T:`, `I:`, `K:`, `Z:`, `G:`, `C:`, `B:` -- is a
    letter. Checked against the real set for GameID 14856: of its 47 lines containing a colon, the
    character before the first one is `M`, `N`, `O`, `P`, `R` or `T`, never a digit. And a definition
    may certainly *begin* with a digit -- that set's first line is `1=1.300.` -- which is exactly why
    the test is digits-then-colon and not digits.

    A line with no id still works, and files written by hand are not expected to carry them. See
    RA_SYNTHETIC_ID_BASE.

    **And the achievement's own title, after the memaddr, separated by a tab**: the full record is
    `<id>:<memaddr>\t<title>`. The notification says what was earned rather than that something was.

    A tab, and the choice is what makes this unambiguous rather than merely convenient. A title may
    contain anything a person types, colons very much included -- "Chapter 1: Beginnings" -- so it
    cannot be another colon-separated field. It cannot be a second line either, because the reader
    treats every line as a definition. But a memaddr has no whitespace anywhere in its syntax, and
    JSON cannot carry a raw tab inside a string -- it arrives as `\t`, two characters -- so a tab
    can appear in exactly one place in the record and means exactly one thing.

    Backward compatible in both directions. A block with no tabs parses as it always did, and a
    hand-written file needs no title.

    **And then two more fields, for the in-game viewer**: the full record is now

        <id>:<memaddr>\t<title>\t<points>\t<description>

    The order is the whole design, and it is chosen so that **degrading the record is truncating
    it**. Every prefix that ends at a tab boundary is itself a valid record:

    | written | means |
    | --- | --- |
    | `<id>:<memaddr>\t<title>\t<points>\t<desc>` | everything fitted |
    | `<id>:<memaddr>\t<title>\t<points>` | the description did not fit |
    | `<id>:<memaddr>\t<title>` | **exactly the format before this change** |
    | `<id>:<memaddr>` | no label at all, the oldest format |

    So the block filling up walks that table upwards, one field at a time, and a reader never has
    to handle a hole in the middle -- which is what putting the description anywhere but last would
    have created. A definition with no description and a points value would have needed an empty
    field, and an empty positional field is the thing this format has already been bitten by: see
    the note on titles ending in digits in raQueueScan().

    It also means **every file written by an older build still parses**, including a cached set on a
    card, because "title only" is a state the new format has a name for rather than a shape it has
    to tolerate.

    Points before description although points is read less often, because points is one to four
    bytes and the description is up to sixty-four. Dropping the expensive field first is the whole
    point of the order, and dropping the cheap one would buy almost nothing.
*/

typedef struct raPatch {
	char* block;          /* where the definitions go, one per line */
	u32   blockMax;       /* bytes for text, terminator excluded */
	u32   used;

	u32   pendingId;      /* the "ID" seen most recently, waiting for its MemAddr */
	/*
	    ...and the "Title" seen most recently, which arrives *before* the MemAddr in each object just
	    as the id does -- `{"ID":1,"Title":"...","Description":"...","MemAddr":"..."` -- so it needs no
	    deferral either.

	    Cleared when a definition commits, so an achievement with no Title field of its own gets no
	    label rather than the previous achievement's. The narrow case that remains is the *first*
	    achievement in a reply: the root object carries the game's own Title before any achievement, so
	    an untitled first achievement would be labelled with the game's name.

	    Left there rather than chased, and the reason is proportion. Every achievement RA sends has a
	    title, the consequence is one wrong *label*, and the id -- which decides what gets awarded to
	    whom -- is not affected. A wrong label and a wrong id are different orders of mistake.
	*/
	u8    titleLength;
	u8    titleFull;      /* the buffer filled, so titleCut is counted only once */
	char  title[RA_PATCH_TITLE_MAX];
	u16   withTitle;      /* definitions written with one */
	u16   titleCut;       /* ...and titles clipped to RA_PATCH_TITLE_MAX on the way in */
	u16   titleNoRoom;    /* ...and labels dropped so the achievement itself could be kept */
	/*
	    The Description and the Points, captured the same way and on the same terms as the title.

	    Both arrive before the MemAddr in each object -- `{"ID":1,"Title":"...","Description":"...",
	    "Points":5,"MemAddr":"..."` -- so neither needs the deferral the flags need, and both are
	    cleared when a definition commits so one achievement never inherits another's.

	    Points is a *number* rather than a string, which is the one place these two differ from the
	    title: it has no closing quote to stop at, so it is read to the first character that is not a
	    digit. `pointsSeen` rather than a zero test, because 0 is a real points value -- the server's
	    own "Unsupported Game Version" notice carries `"Points":0` -- and "zero points" and "no
	    Points field" have to stay different facts.
	*/
	u8    descLength;
	u8    descFull;       /* the buffer filled, so descCut is counted only once */
	char  desc[RA_PATCH_DESC_MAX];
	u16   withDesc;       /* definitions written with one */
	u16   descCut;        /* ...and descriptions clipped to RA_PATCH_DESC_MAX on the way in */
	u16   descNoRoom;     /* ...and descriptions dropped so the achievement itself could be kept */
	u32   points;
	u8    pointsSeen;     /* a Points field arrived, which 0 cannot say on its own */
	u8    pointsBad;      /* ...and it will not fit a u32, so it is treated as absent */
	u16   withPoints;     /* definitions written with one */
	u16   pointsNoRoom;   /* ...and points dropped so the achievement itself could be kept */
	/*
	    How long this game's Rich Presence script is, decoded, and whether one arrived at all.

	    **Counted and thrown away**, which is the whole point of it. Rendering rich presence on the
	    console is reachable -- it is memory reads against a script, which is what this fork already
	    does sixty times a second -- and *sending* it is not, because the game's boot kills the link
	    and dsiwifi's ARM7 half wants 104,148 bytes against 12,636 free. So the open question is not
	    whether it can be evaluated but whether it can be **stored**: the block is 32K less the
	    viewer's index and the pending tally, and a real set already takes 9,662 of it.

	    RA's scripts range from a line to tens of kilobytes depending on how many lookup tables the
	    author used, and no amount of reasoning settles which this project is facing. One counter and
	    one boot does, before a byte of it is designed. `richSeen` because a script of length 0 and no
	    script at all are different answers.
	*/
	u32   richBytes;
	u8    richSeen;
	u16   withId;         /* definitions written with a RetroAchievements id */
	u16   withoutId;      /* ...and without one, which is a set we cannot report on */
	/*
	    Achievements the account already holds, from r=unlocks. Definitions matching one of these are
	    left out of the block entirely rather than staged and skipped later: the block is the scarcest
	    thing here at 88% full, and the arena and the per-frame budget follow it down.

	    What that costs is that the cardengine cannot know they exist, so it could not one day show
	    "30 of 55 earned". Worth the space today and worth writing down, because the fix would be a
	    format change rather than a flag.
	*/
	const u32* skipIds;
	u16   skipCount;
	/*
	    When an achievement in that skip list was earned, packed -- see RA_WHEN_MINUTE. A function
	    rather than a table because only the launcher can answer it: the date comes from
	    r=startsession's reply, which this file has no business knowing about.

	    Null is legal and means no dates are staged, which is what a boot with no network produces.
	*/
	u32 (*whenOf)(u32 id);
	u16   alreadyDone;    /* definitions matching one, which are written for the viewer instead */
	/*
	    ...and what that writing produced. `earned` is the display-only records in the block,
	    `earnedNoRoom` is the ones that did not fit even stripped to a bare id.

	    They are not `kept`, and keeping the two counts apart is the point: `kept` is what the game
	    will run and `earned` is what the menu will show, and a set whose armed half is empty because
	    the player has finished it is a very different reading from one whose armed half is empty
	    because the block overflowed.
	*/
	u16   earned;
	u16   earnedNoRoom;

	u16   oddIds;         /* ids at or above RA_ODD_ID_FROM -- see there */
	u32   oddId;          /* the first of them */
	u16   oddFill;        /* how much of oddContext is written */
	char  oddContext[RA_ODD_CONTEXT_MAX];

	u16   kept;           /* written to the block */
	u16   unofficial;     /* dropped: Flags 5, not part of the published set */
	u16   oddFlags;       /* kept, but Flags was neither 3 nor 5 */
	u16   noFlags;        /* kept, and no Flags field ever arrived */
	u16   tooLong;        /* dropped: longer than RA_PATCH_MEMADDR_MAX */
	u16   cutShort;       /* dropped: the stream ended in the middle of the value */
	u16   empty;          /* dropped: the value was "" */
	u16   dropped;        /* dropped: the block was full */
	u32   wanted;         /* bytes the kept and block-full definitions needed between them */
	u32   longest;        /* longest memaddr seen, decoded -- including ones too long to keep */
	u32   shortest;       /* shortest one that belonged in the block: a fragment as a number */

	/* Scanner state. Carried across chunks, which is the whole point of it being here. */
	u8    state;
	u8    memAt;
	u8    flagsAt;
	u8    idAt;
	u8    titleAt;
	u8    descAt;
	u8    pointsAt;
	u8    richAt;
	u8    idBad;          /* the id being read will not fit a u32; treat the line as id-less */
	u8    oddCapture;     /* copying the reply into oddContext right now */
	u8    escape;
	/*
	    Quoting, tracked in the scanning state and consulted by exactly one rule: whether a `{` starts
	    a JSON object or merely sits inside somebody's Description. See raPatchFeed().
	*/
	u8    scanEscape;
	u8    inString;
	u8    pendingOpen;    /* a value has started, so there is something to commit */
	u8    pendingBad;     /* ...and it overran RA_PATCH_MEMADDR_MAX */
	u8    flagsSeen;
	u32   flags;
	u32   pendingSeen;    /* decoded length, whether or not it was stored */
	u32   pendingLength;  /* how much of it is in pending[] */
	char  pending[RA_PATCH_MEMADDR_MAX];
} raPatch;

/*
    Step 3b. What the launcher had to allocate to compute the hash, reported so the margin
    against the heap is a measurement rather than a hope: rc_hash_nintendo_ds() takes
    max(0xA00, arm9Size, arm7Size) in one block, and the launcher has ~352 K of heap before
    lwip starts and ~191 K after.
*/
typedef struct raHashInfo {
	u32 arm9Size;
	u32 arm7Size;
	u32 bufferBytes;
} raHashInfo;

#if RA_LAUNCHER_WIFI
/*
    The probe itself, on the ARM9 launcher. Never returns: it stops on a summary. See the
    switch above for why that is the design and not a shortcut.
*/
/*
    `cheatsOn` is whether the cheat engine is going to run: the same sum of the three cheat inputs
    that main.arm7.c builds `cheatSizeTotal` from, against RA_CHEATS_MIN_BYTES. **Not** whether a
    cheat file exists, which is what it used to be and which was wrong twice -- see the call in
    main.cpp for both failures.

    Passed in rather than read here because this file knows nothing about nds-bootstrap's
    configuration, and because what hardcore requires is a *list* of such facts -- see
    raWifiHardcoreRefused().

    It is the launcher's best answer rather than the final one: the bootloader clears the staged
    session's hardcore flag if it installs the engine after all.
*/
void raWifiProbe(bool sdFound, const char* ndsPath, bool cheatsOn);

/*
    The RetroAchievements hash of a DS ROM, as rcheevos computes it. 32 lowercase hex digits
    and a terminator. False leaves hash empty and raHashLastError() explaining why.
*/
bool        raHashRom(const char* path, char hash[33], raHashInfo* info);
const char* raHashLastError(void);

/* Step 3c. The config file, and what may be said about a secret out loud. */
bool        raConfigRead(const char* path, raConfig* cfg);
const char* raConfigRedact(const char* secret);

/* Step 3c. The transport. */
bool        raNetUrlEncode(const char* in, char* out, size_t outSize);
int         raNetHttpGet(const char* host, const char* path, char* out, int outSize,
                         raNetProgress* p);
const char* raNetBody(const char* response);
bool        raNetJsonString(const char* json, const char* key, char* out, size_t outSize);
bool        raNetJsonNumber(const char* json, const char* key, u32* out);
/*
    Content-Length and chunked, out of a raw response's headers, bounded so a body cannot supply
    either. Pure and host-tested -- see the note on the definition.
*/
u32         raNetHeaderLength(const char* response, u32 headerEnd);
int         raNetHeaderChunked(const char* response, u32 headerEnd);
bool        raNetJsonTrue(const char* json, const char* key);
/*
    Step 6's prerequisite: `"UserUnlocks":[93119,93120,...]` into an array of ids. Returns how many
    were read, or -1 if the key is absent -- and the difference matters, because an empty list is a
    real answer (a player who has earned nothing) while a missing key means the request failed and
    every definition must be staged.
*/
int         raNetJsonIdList(const char* json, const char* key, u32* out, int max);
int         raNetJsonObjectField(const char* json, const char* key, const char* field,
                                 u32* out, int max);

/*
    Step 3d. The streaming half: reset, feed the socket's bytes in whatever sizes they arrive,
    and the sink sees the body with the headers and any chunk framing already gone.
    raNetStreamFeed() has no I/O in it and is checked on a host at every split point.
*/
void raNetStreamReset(raNetStream* s, raNetSink sink, void* ctx);
void raNetStreamFeed(raNetStream* s, const char* data, int length);
int  raNetHttpGetStream(const char* host, const char* path, raNetSink sink, void* ctx,
                        raNetProgress* p);

/* Step 3d. The scanner. Also pure, also fed in arbitrary pieces. */
void raPatchReset(raPatch* p, char* block, u32 blockMax);
void raPatchFeed(void* p, const char* data, int length);
void raPatchFinish(raPatch* p);

/*
    Step 3a. The unlock queue, and every part of it that does not touch the network or the SD card.

    Split out for the same reason ra_patch.c was: this is the logic that is expensive to debug on a
    console and cheap to pin on a PC. In particular raQueueSign() -- a wrong signature is rejected by
    the server with a message that does not say "your hash is wrong", so it is pinned against digests
    computed by coreutils rather than by this code.
*/
void raQueueScan(raQueue* q, const char* text, int length);
/*
    `keep` is indices into `q`, not ids, so a record keeps everything it arrived with -- its stamp and
    the game it came from -- rather than only the parts the caller thought to carry across.
*/
int  raQueuePack(const raQueue* q, const int* keep, int keepCount, char* out, int outSize);

/*
    What the in-game menu shows: one line per game, three fields.

        CONTRA4          2      today
        MARIOKART        2  yesterday

    **Counts rather than a list of achievements**, and that is what makes it cheap. Achievement titles
    only exist in the staged definitions of the game that is *running*, so a list could name the current
    game's unlocks and could only ever print bare ids for every other game -- rows of two classes, and
    the ugly class is exactly the one this feature exists for. A count reads the same for every game.
    The player was already told which achievement by the notification, with its name, when it fired;
    what the menu answers is whether it is safe and how much is waiting.

    **`waitDays` rather than a date**, because a date cannot say what this column is for without also
    picking a convention. Five characters cannot distinguish 08/09 the ninth of August from the eighth
    of September, and the column's whole job is "how long has this been stuck". A count of days is
    unambiguous in any locale and needs no month table.

    It is also the only form the menu can afford: the in-game menu has no date. sharedAddr[7]/[8] carry
    hours and minutes, and only while the menu is open. The launcher has a real clock -- it already
    reads it for `o=` -- so the subtraction happens there, at boot, and the menu prints an integer. A
    session lasts hours, so a number computed at boot is still right when the menu opens.
*/

/*
    Group a queue by game. Pure, and the reason it is its own function rather than part of the staging
    code: the grouping, the day arithmetic and the overflow behaviour are all things a host test can
    pin, and none of them can be checked on a console without earning achievements in several games.

    `now` is Unix seconds. A record whose stamp is unusable still counts -- it is owed -- but never
    contributes to `waitDays`: an unknown age must not be reported as zero days, which the menu prints
    as "today" and which is a claim the record never made. A record with no stamp at all has no game
    either, the fields being positional, so it lands in `unnamed` rather than under a blank title.
*/
void raQueueTally(const raQueue* q, u32 now, raPendingBlock* out);

/*
    A record's `YYYYMMDDhhmmss` and Unix seconds, each way. `raQueueStampToUnix()` reads 14 bytes and
    returns 0 for any date it will not vouch for; `raQueueUnixToStamp()` writes exactly 14 bytes and
    appends no terminator. Local time is treated as UTC at both ends, which cancels out of the
    difference `o=` actually sends -- see the note in ra_queue.c.
*/
u32  raQueueStampToUnix(const char* digits);
void raQueueUnixToStamp(u32 when, char* out);

/*
    v=, the parameter that makes the server believe the unlock came from an account rather than from
    anyone who knows an id. md5 of the id, the username and the hardcore flag, concatenated as
    decimal text with no separators -- the formula is rcheevos'
    rc_api_init_award_achievement_request_hosted(), read out of the vendored copy rather than
    remembered. `out` needs 33 bytes.

    `seconds` is what goes in `o=`, and it changes the digest as well as the URL: when it is non-zero
    the id is appended a *second* time and then the seconds. Sending `o=` without that is a refusal
    the server explains as nothing in particular. Pass 0 for no `o=`, which signs exactly as before.

    The username must be the *raw* one, not the URL-encoded form that goes in u=: the server hashes
    what it decoded. Getting that backwards is a signature failure on any account with a space in it
    and on no other, which is exactly the kind of bug that ships.
*/
void raQueueSign(u32 id, const char* username, int hardcore, u32 seconds, char* out);

/* The ARM7 half: hand this CPU to dsiwifi. One call, and where it goes matters. */
void raWifiInstall(void);

/*
    ...and take it back. Called from the ARM7's idle loop rather than from the FIFO handler that
    asks for it, because wifi_card_deinit() writes SDIO registers and polls for the controller to
    answer -- a bounded wait, but not one to take inside an interrupt on a CPU that is about to be
    overwritten. The handler sets a flag; this does the work.

    A no-op until asked, so the loop pays a compare per FIFO wake-up.
*/
void raWifiPoll(void);

/*
    retail/arm9/source/conf_sd.cpp -- stage sd:/_nds/nds-bootstrap/ra_achievements.txt into the
    definitions block. Called once during loadFromSD(), and again by stage 12 when a fetch fails,
    because the fetch streams into that same block and destroys the file's text on its way.
*/
void loadRaDefinitions(void);

/*
    Mode 2's teardown, on the ARM9: stop dsiwifi's TIMER3 and its FIFO handler, ask the ARM7 to run
    wifi_card_deinit(), and wait a bounded time for it to say it did.

    Returns false if the ARM7 never answered, which is the one case where booting the game is worse
    than not booting it -- an ARM7 still taking SDIO interrupts when the bootloader overwrites its
    code. The caller decides; this only reports.
*/
bool raWifiShutdown(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RA_WIFI_H */
