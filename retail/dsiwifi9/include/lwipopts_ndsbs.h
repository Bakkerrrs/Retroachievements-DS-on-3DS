/*
    lwip's options for nds-bootstrap's launcher: dsiwifi's own, with the pools cut to fit.

    Step 3 needs an IP stack in the launcher, and step 2 measured why that is not a matter of
    just linking one: dsiwifi configures lwip with `PBUF_POOL_SIZE 512` and
    `MEMP_MEM_MALLOC 0`, so the pools are static arrays, and `memp_memory_PBUF_POOL_base`
    alone is **784,387 bytes** of `.bss`. The launcher's whole ARM9 link region is 753,664
    bytes. The pool is bigger than the region it would have to live in.

    Those numbers are right for a homebrew app that owns all 16 MB. They are not a bug to be
    reported upstream and they are not something to argue with -- they are simply not this
    program's budget. What this program does is one HTTP GET before a game boots.

    ## Why the launcher cannot just be given more room instead

    Checked before writing any of this, because it would have been the cheaper fix.
    `retail/arm9/ds_arm9_ndsbs.mem` puts the launcher at 0x02280000-0x02338000, and it is
    boxed in on both sides by things the launcher itself uses: `IMAGES_LOCATION` at
    0x02338000, where `conf_sd.cpp` decompresses the boot images the bootloader later
    displays, and `CARDENGINE_ARM9_SLOT2HEAP_LOCATION_BUFFERED` at 0x0227F800 just below.
    Growing the region upward would put the launcher's own `.bss` on top of the images it
    writes. Relocating the launcher entirely is a different and much larger change.

    ## How the override reaches lwip at all, which is not obvious

    lwip's `opt.h` does `#include "lwipopts.h"`, and for a quoted include the compiler
    searches the *including file's own directory* first -- where dsiwifi's `lwipopts.h`
    already sits, next to `opt.h`. **No -I or -iquote path can win that race**, which is worth
    stating because it is the first three things anyone would try.

    What works is the header guard. This file is force-included ahead of every translation
    unit of `retail/dsiwifi9` with `-include`, it pulls in dsiwifi's options by explicit
    relative path -- which defines their `__LWIPOPTS_H__` -- and then overrides. By the time
    `opt.h` asks for `lwipopts.h`, the guard makes it a no-op and these values stand.

    Including their file rather than copying it is the point: everything not listed below
    tracks the submodule, and a bump changes their side without silently leaving ours stale.

    Scoped to this library's build and nowhere else, so nothing in the launcher's own
    translation units sees any of it.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef LWIPOPTS_NDSBS_H
#define LWIPOPTS_NDSBS_H

/* Theirs first, by path, so the guard is set and every unlisted option is still theirs. */
#include "../../../libs/dsiwifi/include/lwip/lwipopts.h"

/*
    The pool that decides it. 512 buffers of PBUF_POOL_BUFSIZE (1,516 at their 1460-byte MSS)
    is 760 K; 32 is 48 K.

    32 rather than 4 because this is the *receive* path and running it dry stalls a transfer
    rather than failing it -- a bug that would look like the network being slow. The ceiling
    on what can actually be in flight is `TCP_WND`, which dsiwifi sets to two MSS, plus
    whatever DHCP and DNS hold briefly. Each received frame is at most `IF_MTU_SIZE` (1,392)
    and so fits one buffer. 32 is roughly an order of magnitude over the need, and still
    one twentieth of the cost.
*/
#undef  PBUF_POOL_SIZE
#define PBUF_POOL_SIZE              32

/* References to memory lwip does not own -- 16 bytes each, so 1024 was never the problem. */
#undef  MEMP_NUM_PBUF
#define MEMP_NUM_PBUF               64

/*
    One TCP connection to one host, plus DHCP and DNS on UDP. Sized for that with room, not
    for a server: `MEMP_NUM_TCP_SEG` is the one worth not trimming hard, since `TCP_SND_BUF`
    is four MSS and segments are held until acknowledged.
*/
#undef  MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB            8
#undef  MEMP_NUM_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_PCB_LISTEN     2
#undef  MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG            24
#undef  MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB            8
#undef  MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN            8
#undef  MEMP_NUM_REASSDATA
#define MEMP_NUM_REASSDATA          8
#undef  MEMP_NUM_ARP_QUEUE
#define MEMP_NUM_ARP_QUEUE          8

/*
    NetBIOS name service is started by wifi_host_lwip_init() and answers name queries for the
    console. Harmless, and not what a launcher fetching one URL is for -- but left alone
    deliberately: turning it off would mean dsiwifi's own call to netbiosns_init() no longer
    links, which is a change to their code path rather than to their sizing. Sizing is what
    this file is for.
*/

#endif /* LWIPOPTS_NDSBS_H */
