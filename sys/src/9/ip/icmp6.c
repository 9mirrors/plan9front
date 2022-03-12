/*
 * Internet Control Message Protocol for IPv6
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "ip.h"
#include "ipv6.h"

enum
{
	InMsgs6,
	InErrors6,
	OutMsgs6,
	CsumErrs6,
	LenErrs6,
	HlenErrs6,
	HoplimErrs6,
	IcmpCodeErrs6,
	TargetErrs6,
	OptlenErrs6,
	AddrmxpErrs6,
	RouterAddrErrs6,

	Nstats6,
};

enum {
	ICMP_USEAD6	= 40,
};

enum {
	Oflag	= 1<<5,
	Sflag	= 1<<6,
	Rflag	= 1<<7,
};

enum {
	/* ICMPv6 types */
	EchoReply	= 0,
	UnreachableV6	= 1,
	PacketTooBigV6	= 2,
	TimeExceedV6	= 3,
	SrcQuench	= 4,
	ParamProblemV6	= 4,
	Redirect	= 5,
	EchoRequest	= 8,
	TimeExceed	= 11,
	InParmProblem	= 12,
	Timestamp	= 13,
	TimestampReply	= 14,
	InfoRequest	= 15,
	InfoReply	= 16,
	AddrMaskRequest = 17,
	AddrMaskReply   = 18,
	EchoRequestV6	= 128,
	EchoReplyV6	= 129,
	RouterSolicit	= 133,
	RouterAdvert	= 134,
	NbrSolicit	= 135,
	NbrAdvert	= 136,
	RedirectV6	= 137,

	Maxtype6	= 137,
};

enum {
	MinAdvise	= IP6HDR+4,	/* minimum needed for us to advise another protocol */ 
};

/* on-the-wire packet formats */
typedef struct IPICMP IPICMP;
typedef struct Ndpkt Ndpkt;
typedef struct NdiscC NdiscC;

/* we do this to avoid possible struct padding  */
#define ICMPHDR \
	IPV6HDR; \
	uchar	type; \
	uchar	code; \
	uchar	cksum[2]; \
	uchar	icmpid[2]; \
	uchar	seq[2]

struct IPICMP {
	ICMPHDR;
	uchar	payload[];
};

#define IPICMPSZ offsetof(IPICMP, payload[0])

struct NdiscC {
	ICMPHDR;
	uchar	target[IPaddrlen];
	uchar	payload[];
};

#define NDISCSZ offsetof(NdiscC, payload[0])

struct Ndpkt {
	ICMPHDR;
	uchar	target[IPaddrlen];
	uchar	otype;
	uchar	olen;		/* length in units of 8 octets(incl type, code),
				 * 1 for IEEE 802 addresses */
	uchar	lnaddr[6];	/* link-layer address */
	uchar	payload[];
};

#define NDPKTSZ offsetof(Ndpkt, payload[0])

typedef struct Icmppriv6
{
	ulong	stats[Nstats6];

	/* message counts */
	ulong	in[Maxtype6+1];
	ulong	out[Maxtype6+1];
} Icmppriv6;

typedef struct Icmpcb6
{
	uchar	headers;
} Icmpcb6;

char *icmpnames6[Maxtype6+1] =
{
[EchoReply]		"EchoReply",
[UnreachableV6]		"UnreachableV6",
[PacketTooBigV6]	"PacketTooBigV6",
[TimeExceedV6]		"TimeExceedV6",
[SrcQuench]		"SrcQuench",
[Redirect]		"Redirect",
[EchoRequest]		"EchoRequest",
[TimeExceed]		"TimeExceed",
[InParmProblem]		"InParmProblem",
[Timestamp]		"Timestamp",
[TimestampReply]	"TimestampReply",
[InfoRequest]		"InfoRequest",
[InfoReply]		"InfoReply",
[AddrMaskRequest]	"AddrMaskRequest",
[AddrMaskReply]		"AddrMaskReply",
[EchoRequestV6]		"EchoRequestV6",
[EchoReplyV6]		"EchoReplyV6",
[RouterSolicit]		"RouterSolicit",
[RouterAdvert]		"RouterAdvert",
[NbrSolicit]		"NbrSolicit",
[NbrAdvert]		"NbrAdvert",
[RedirectV6]		"RedirectV6",
};

static char *statnames6[Nstats6] =
{
[InMsgs6]	"InMsgs",
[InErrors6]	"InErrors",
[OutMsgs6]	"OutMsgs",
[CsumErrs6]	"CsumErrs",
[LenErrs6]	"LenErrs",
[HlenErrs6]	"HlenErrs",
[HoplimErrs6]	"HoplimErrs",
[IcmpCodeErrs6]	"IcmpCodeErrs",
[TargetErrs6]	"TargetErrs",
[OptlenErrs6]	"OptlenErrs",
[AddrmxpErrs6]	"AddrmxpErrs",
[RouterAddrErrs6]	"RouterAddrErrs",
};

static char *unreachcode[] =
{
[Icmp6_no_route]	"no route to destination",
[Icmp6_ad_prohib]	"comm with destination administratively prohibited",
[Icmp6_out_src_scope]	"beyond scope of source address",
[Icmp6_adr_unreach]	"address unreachable",
[Icmp6_port_unreach]	"port unreachable",
[Icmp6_gress_src_fail]	"source address failed ingress/egress policy",
[Icmp6_rej_route]	"reject route to destination",
[Icmp6_unknown]		"icmp unreachable: unknown code",
};

static void icmpkick6(void *x, Block *bp);

static void
icmpcreate6(Conv *c)
{
	c->rq = qopen(64*1024, Qmsg, 0, c);
	c->wq = qbypass(icmpkick6, c);
}

static void
set_cksum(Block *bp)
{
	IPICMP *p = (IPICMP *)(bp->rp);
	int n = blocklen(bp);

	hnputl(p->vcf, 0);  	/* borrow IP header as pseudoheader */
	hnputs(p->ploadlen, n - IP6HDR);
	p->proto = 0;
	p->ttl = ICMPv6;	/* ttl gets set later */
	hnputs(p->cksum, 0);
	hnputs(p->cksum, ptclcsum(bp, 0, n));
	p->proto = ICMPv6;
}

static Block *
newIPICMP(int packetlen)
{
	Block *nbp;

	nbp = allocb(packetlen);
	nbp->wp += packetlen;
	memset(nbp->rp, 0, packetlen);
	return nbp;
}

static void
icmpadvise6(Proto *icmp, Block *bp, char *msg)
{
	ushort recid;
	Conv **c, *s;
	IPICMP *p;

	p = (IPICMP *)bp->rp;
	recid = nhgets(p->icmpid);

	for(c = icmp->conv; (s = *c) != nil; c++){
		if(s->lport == recid)
		if(ipcmp(s->laddr, p->src) == 0)
		if(ipcmp(s->raddr, p->dst) == 0){
			if(s->ignoreadvice)
				break;
			qhangup(s->rq, msg);
			qhangup(s->wq, msg);
			break;
		}
	}
	freeblist(bp);
}

static void
icmpkick6(void *x, Block *bp)
{
	uchar laddr[IPaddrlen], raddr[IPaddrlen];
	Conv *c = x;
	IPICMP *p;
	Icmppriv6 *ipriv = c->p->priv;
	Icmpcb6 *icb = (Icmpcb6*)c->ptcl;

	if(bp == nil)
		return;

	if(icb->headers==6) {
		/* get user specified addresses */
		bp = pullupblock(bp, ICMP_USEAD6);
		if(bp == nil)
			return;
		bp->rp += 8;
		ipmove(laddr, bp->rp);
		bp->rp += IPaddrlen;
		ipmove(raddr, bp->rp);
		bp->rp += IPaddrlen;
		bp = padblock(bp, IP6HDR);
	}

	if(BLEN(bp) < IPICMPSZ){
		freeblist(bp);
		return;
	}
	p = (IPICMP *)(bp->rp);
	if(icb->headers == 6) {
		ipmove(p->dst, raddr);
		ipmove(p->src, laddr);
	} else {
		ipmove(p->dst, c->raddr);
		ipmove(p->src, c->laddr);
		hnputs(p->icmpid, c->lport);
	}

	set_cksum(bp);
	p->vcf[0] = 0x06 << 4;
	if(p->type <= Maxtype6)
		ipriv->out[p->type]++;
	ipoput6(c->p->f, bp, 0, c->ttl, c->tos, nil);
}

static char*
icmpctl6(Conv *c, char **argv, int argc)
{
	Icmpcb6 *icb;

	icb = (Icmpcb6*) c->ptcl;
	if(argc==1 && strcmp(argv[0], "headers")==0) {
		icb->headers = 6;
		return nil;
	}
	return "unknown control request";
}

static void
goticmpkt6(Proto *icmp, Block *bp, int muxkey)
{
	ushort recid;
	uchar *addr;
	Conv **c, *s;
	IPICMP *p = (IPICMP *)bp->rp;

	if(muxkey == 0) {
		recid = nhgets(p->icmpid);
		addr = p->src;
	} else {
		recid = muxkey;
		addr = p->dst;
	}
	for(c = icmp->conv; (s = *c) != nil; c++){
		if(s->lport == recid)
		if(ipcmp(s->laddr, p->dst) == 0 || ipcmp(s->raddr, addr) == 0)
			qpass(s->rq, copyblock(bp, blocklen(bp)));
	}
	freeblist(bp);
}

static Block *
mkechoreply6(Block *bp, Ipifc *ifc)
{
	uchar addr[IPaddrlen];
	IPICMP *p = (IPICMP *)(bp->rp);

	if(isv6mcast(p->src))
		return nil;
	ipmove(addr, p->src);
	if(!isv6mcast(p->dst))
		ipmove(p->src, p->dst);
	else if (!ipv6local(ifc, p->src, 0, addr))
		return nil;
	ipmove(p->dst, addr);
	p->type = EchoReplyV6;
	set_cksum(bp);
	return bp;
}

/*
 * sends out an ICMPv6 neighbor solicitation
 * 	suni == SRC_UNSPEC or SRC_UNI,
 *	tuni == TARG_MULTI => multicast for address resolution,
 * 	and tuni == TARG_UNI => neighbor reachability.
 */
void
icmpns(Fs *f, uchar* src, int suni, uchar* targ, int tuni, uchar* mac)
{
	Block *nbp;
	Ndpkt *np;
	Proto *icmp = f->t2p[ICMPv6];
	Icmppriv6 *ipriv = icmp->priv;

	nbp = newIPICMP(NDPKTSZ);
	np = (Ndpkt*) nbp->rp;

	if(suni == SRC_UNSPEC)
		ipmove(np->src, v6Unspecified);
	else
		ipmove(np->src, src);

	if(tuni == TARG_UNI)
		ipmove(np->dst, targ);
	else
		ipv62smcast(np->dst, targ);

	np->type = NbrSolicit;
	np->code = 0;
	ipmove(np->target, targ);
	if(suni != SRC_UNSPEC) {
		np->otype = SRC_LLADDR;
		np->olen = 1;		/* 1+1+6 = 8 = 1 8-octet */
		memmove(np->lnaddr, mac, sizeof(np->lnaddr));
	} else
		nbp->wp -= NDPKTSZ - NDISCSZ;

	set_cksum(nbp);
	np = (Ndpkt*)nbp->rp;
	np->ttl = HOP_LIMIT;
	np->vcf[0] = 0x06 << 4;
	ipriv->out[NbrSolicit]++;
	netlog(f, Logicmp, "sending neighbor solicitation %I\n", targ);
	ipoput6(f, nbp, 0, MAXTTL, DFLTTOS, nil);
}

/*
 * sends out an ICMPv6 neighbor advertisement. pktflags == RSO flags.
 */
void
icmpna(Fs *f, uchar* src, uchar* dst, uchar* targ, uchar* mac, uchar flags)
{
	Block *nbp;
	Ndpkt *np;
	Proto *icmp = f->t2p[ICMPv6];
	Icmppriv6 *ipriv = icmp->priv;

	nbp = newIPICMP(NDPKTSZ);
	np = (Ndpkt*)nbp->rp;

	ipmove(np->src, src);
	ipmove(np->dst, dst);

	np->type = NbrAdvert;
	np->code = 0;
	np->icmpid[0] = flags;
	ipmove(np->target, targ);

	np->otype = TARGET_LLADDR;
	np->olen = 1;
	memmove(np->lnaddr, mac, sizeof(np->lnaddr));

	set_cksum(nbp);
	np = (Ndpkt*)nbp->rp;
	np->ttl = HOP_LIMIT;
	np->vcf[0] = 0x06 << 4;
	ipriv->out[NbrAdvert]++;
	netlog(f, Logicmp, "sending neighbor advertisement %I\n", targ);
	ipoput6(f, nbp, 0, MAXTTL, DFLTTOS, nil);
}

void
icmphostunr6(Fs *f, Ipifc *ifc, Block *bp, int code, int tome)
{
	int osz = BLEN(bp);
	int sz = MIN(IPICMPSZ + osz, v6MINTU);
	Block *nbp;
	IPICMP *np;
	Ip6hdr *p;
	Proto *icmp = f->t2p[ICMPv6];
	Icmppriv6 *ipriv = icmp->priv;
	uchar ia[IPaddrlen];

	p = (Ip6hdr *)bp->rp;
	if(isv6mcast(p->dst) || isv6mcast(p->src) || !ipv6local(ifc, ia, 0, p->src))
		return;

	netlog(f, Logicmp, "send icmphostunr %I -> src %I dst %I\n",
		ia, p->src, p->dst);

	nbp = newIPICMP(sz);
	np = (IPICMP *)nbp->rp;
	ipmove(np->src, ia);
	ipmove(np->dst, p->src);
	np->type = UnreachableV6;
	np->code = code;
	memmove(nbp->rp + IPICMPSZ, bp->rp, sz - IPICMPSZ);
	set_cksum(nbp);
	np->ttl = HOP_LIMIT;
	np->vcf[0] = 0x06 << 4;
	ipriv->out[UnreachableV6]++;

	if(tome)
		ipiput6(f, ifc, nbp);
	else 
		ipoput6(f, nbp, 0, MAXTTL, DFLTTOS, nil);
}

void
icmpttlexceeded6(Fs *f, Ipifc *ifc, Block *bp)
{
	int osz = BLEN(bp);
	int sz = MIN(IPICMPSZ + osz, v6MINTU);
	Block *nbp;
	IPICMP *np;
	Ip6hdr *p;
	Proto *icmp = f->t2p[ICMPv6];
	Icmppriv6 *ipriv = icmp->priv;
	uchar ia[IPaddrlen];

	p = (Ip6hdr *)bp->rp;
	if(isv6mcast(p->dst) || isv6mcast(p->src) || !ipv6local(ifc, ia, 0, p->src))
		return;

	netlog(f, Logicmp, "send icmpttlexceeded6 %I -> src %I dst %I\n",
		ia, p->src, p->dst);

	nbp = newIPICMP(sz);
	np = (IPICMP *) nbp->rp;
	ipmove(np->src, ia);
	ipmove(np->dst, p->src);
	np->type = TimeExceedV6;
	np->code = 0;
	memmove(nbp->rp + IPICMPSZ, bp->rp, sz - IPICMPSZ);
	set_cksum(nbp);
	np->ttl = HOP_LIMIT;
	np->vcf[0] = 0x06 << 4;
	ipriv->out[TimeExceedV6]++;
	ipoput6(f, nbp, 0, MAXTTL, DFLTTOS, nil);
}

void
icmppkttoobig6(Fs *f, Ipifc *ifc, Block *bp)
{
	int osz = BLEN(bp);
	int sz = MIN(IPICMPSZ + osz, v6MINTU);
	Block *nbp;
	IPICMP *np;
	Ip6hdr *p;
	Proto *icmp = f->t2p[ICMPv6];
	Icmppriv6 *ipriv = icmp->priv;
	uchar ia[IPaddrlen];

	p = (Ip6hdr *)bp->rp;
	if(isv6mcast(p->dst) || isv6mcast(p->src) || !ipv6local(ifc, ia, 0, p->src))
		return;

	netlog(f, Logicmp, "send icmppkttoobig6 %I -> src %I dst %I\n",
		ia, p->src, p->dst);

	nbp = newIPICMP(sz);
	np = (IPICMP *)nbp->rp;
	ipmove(np->src, ia);
	ipmove(np->dst, p->src);
	np->type = PacketTooBigV6;
	np->code = 0;
	hnputl(np->icmpid, ifc->maxtu - ifc->m->hsize);
	memmove(nbp->rp + IPICMPSZ, bp->rp, sz - IPICMPSZ);
	set_cksum(nbp);
	np->ttl = HOP_LIMIT;
	np->vcf[0] = 0x06 << 4;
	ipriv->out[PacketTooBigV6]++;
	ipoput6(f, nbp, 0, MAXTTL, DFLTTOS, nil);
}

/*
 * RFC 2461, pages 39-40, pages 57-58.
 */
static int
valid(Proto *icmp, Ipifc *, Block *bp, Icmppriv6 *ipriv)
{
	int sz, osz, unsp, ttl;
	int pktsz = BLEN(bp);
	uchar *packet = bp->rp;
	IPICMP *p = (IPICMP *) packet;
	Ndpkt *np;

	if(pktsz < IPICMPSZ) {
		ipriv->stats[HlenErrs6]++;
		netlog(icmp->f, Logicmp, "icmp hlen %d\n", pktsz);
		goto err;
	}

	/* Rather than construct explicit pseudoheader, overwrite IPv6 header */
	if(p->proto != ICMPv6) {
		/* This code assumes no extension headers!!! */
		netlog(icmp->f, Logicmp, "icmp error: extension header\n");
		goto err;
	}
	memset(packet, 0, 4);
	ttl = p->ttl;
	p->ttl = p->proto;
	p->proto = 0;
	if(ptclcsum(bp, 0, pktsz)) {
		ipriv->stats[CsumErrs6]++;
		netlog(icmp->f, Logicmp, "icmp checksum error\n");
		goto err;
	}
	p->proto = p->ttl;
	p->ttl = ttl;

	/* additional tests for some pkt types */
	if (p->type == NbrSolicit   || p->type == NbrAdvert ||
	    p->type == RouterAdvert || p->type == RouterSolicit ||
	    p->type == RedirectV6) {
		if(p->ttl != HOP_LIMIT) {
			ipriv->stats[HoplimErrs6]++;
			goto err;
		}
		if(p->code != 0) {
			ipriv->stats[IcmpCodeErrs6]++;
			goto err;
		}

		switch (p->type) {
		case NbrSolicit:
		case NbrAdvert:
			np = (Ndpkt*) p;
			if(isv6mcast(np->target)) {
				ipriv->stats[TargetErrs6]++;
				goto err;
			}
			if(optexsts(np) && np->olen == 0) {
				ipriv->stats[OptlenErrs6]++;
				goto err;
			}

			if (p->type == NbrSolicit &&
			    ipcmp(np->src, v6Unspecified) == 0)
				if(!issmcast(np->dst) || optexsts(np)) {
					ipriv->stats[AddrmxpErrs6]++;
					goto err;
				}

			if(p->type == NbrAdvert)
				if(isv6mcast(np->dst) &&
				    (nhgets(np->icmpid) & Sflag)){
					ipriv->stats[AddrmxpErrs6]++;
					goto err;
				}
			break;

		case RouterAdvert:
			if(pktsz - IP6HDR < 16) {
				ipriv->stats[HlenErrs6]++;
				goto err;
			}
			if(!islinklocal(p->src)) {
				ipriv->stats[RouterAddrErrs6]++;
				goto err;
			}
			sz = IPICMPSZ + 8;
			while (sz+8 <= pktsz) {
				osz = packet[sz+1];
				if(osz <= 0) {
					ipriv->stats[OptlenErrs6]++;
					goto err;
				}
				sz += 8*osz;
			}
			break;

		case RouterSolicit:
			if(pktsz - IP6HDR < 8) {
				ipriv->stats[HlenErrs6]++;
				goto err;
			}
			unsp = (ipcmp(p->src, v6Unspecified) == 0);
			sz = IPICMPSZ + 8;
			while (sz+8 <= pktsz) {
				osz = packet[sz+1];
				if(osz <= 0 ||
				    (unsp && packet[sz] == SRC_LLADDR)) {
					ipriv->stats[OptlenErrs6]++;
					goto err;
				}
				sz += 8*osz;
			}
			break;

		case RedirectV6:
			/* to be filled in */
			break;

		default:
			goto err;
		}
	}
	return 1;
err:
	ipriv->stats[InErrors6]++;
	return 0;
}

static int
targettype(Fs *f, Ipifc *ifc, uchar *target)
{
	Iplifc *lifc;
	int t;

	if((lifc = iplocalonifc(ifc, target)) != nil)
		t = lifc->tentative? Tunitent: Tunirany;
	else if(ipproxyifc(f, ifc, target))
		t = Tuniproxy;
	else
		t = 0;
	return t;
}

static void
icmpiput6(Proto *icmp, Ipifc *ifc, Block *bp)
{
	char *msg, m2[128];
	uchar pktflags;
	uchar ia[IPaddrlen];
	Block *r;
	IPICMP *p;
	Icmppriv6 *ipriv = icmp->priv;
	Iplifc *lifc;
	Ndpkt* np;
	Proto *pr;

	bp = concatblock(bp);
	p = (IPICMP*)bp->rp;

	if(!valid(icmp, ifc, bp, ipriv) || p->type > Maxtype6)
		goto raise;

	ipriv->in[p->type]++;

	switch(p->type) {
	case EchoRequestV6:
		r = mkechoreply6(bp, ifc);
		if(r == nil)
			goto raise;
		ipriv->out[EchoReply]++;
		ipoput6(icmp->f, r, 0, MAXTTL, DFLTTOS, nil);
		break;

	case UnreachableV6:
		if(p->code >= nelem(unreachcode))
			msg = unreachcode[Icmp6_unknown];
		else
			msg = unreachcode[p->code];
	Advise:
		bp->rp += IPICMPSZ;
		if(BLEN(bp) < MinAdvise){
			ipriv->stats[LenErrs6]++;
			goto raise;
		}
		p = (IPICMP *)bp->rp;
		/* get rid of fragment header if this is the first fragment */
		if((p->vcf[0] & 0xF0) == IP_VER6 && p->proto == FH && BLEN(bp) >= MinAdvise+IP6FHDR && MinAdvise > IP6HDR){
			Fraghdr6 *fh = (Fraghdr6*)(bp->rp + IP6HDR);
			if((nhgets(fh->offsetRM) & ~7) == 0){	/* first fragment */
				p->proto = fh->nexthdr;
				/* copy down payload over fragment header */
				bp->rp += IP6HDR;
				bp->wp -= IP6FHDR;
				memmove(bp->rp, bp->rp+IP6FHDR, BLEN(bp));
				hnputs(p->ploadlen, BLEN(bp));
				bp->rp -= IP6HDR;
			}
		}
		if((p->vcf[0] & 0xF0) == IP_VER6 && p->proto != FH){
			pr = Fsrcvpcolx(icmp->f, p->proto);
			if(pr != nil && pr->advise != nil) {
				netlog(icmp->f, Logicmp, "advising %s!%I -> %I: %s\n", pr->name, p->src, p->dst, msg);
				(*pr->advise)(pr, bp, msg);
				return;
			}
		}
		bp->rp -= IPICMPSZ;
		goticmpkt6(icmp, bp, 0);
		break;

	case TimeExceedV6:
		if(p->code == 0){
			snprint(msg = m2, sizeof m2, "ttl exceeded at %I", p->src);
			goto Advise;
		}
		if(p->code == 1){
			snprint(msg = m2, sizeof m2, "frag time exceeded at %I", p->src);
			goto Advise;
		}
		goticmpkt6(icmp, bp, 0);
		break;

	case PacketTooBigV6:
		snprint(msg = m2, sizeof(m2), "packet too big for %lud mtu at %I",
			(ulong)nhgetl(p->icmpid), p->src);
		goto Advise;

	case RouterAdvert:
	case RouterSolicit:
		goticmpkt6(icmp, bp, p->type);
		break;

	case NbrSolicit:
		np = (Ndpkt*)p;
		pktflags = 0;
		if(ifc->sendra6)
			pktflags |= Rflag;
		switch (targettype(icmp->f, ifc, np->target)) {
		case Tunirany:
			pktflags |= Oflag;
			/* fall through */

		case Tuniproxy:
			if(ipv6local(ifc, ia, 0, np->src)) {
				if(arpenter(icmp->f, V6, np->src, np->lnaddr, 8*np->olen-2, ia, ifc, 0) < 0)
					break;
				pktflags |= Sflag;
			} else
				ipmove(ia, np->target);
			icmpna(icmp->f, ia, (pktflags & Sflag)? np->src: v6allnodesL,
				np->target, ifc->mac, pktflags);
			break;
		case Tunitent:
			/*
			 * not clear what needs to be done. send up
			 * an icmp mesg saying don't use this address?
			 */
			break;
		}
		freeblist(bp);
		break;

	case NbrAdvert:
		np = (Ndpkt*)p;

		/*
		 * if the target address matches one of the local interface
		 * addresses and the local interface address has tentative bit
		 * set, insert into ARP table. this is so the duplicate address
		 * detection part of ipconfig can discover duplication through
		 * the arp table.
		 */
		lifc = iplocalonifc(ifc, np->target);
		if(lifc != nil && lifc->tentative)
			arpenter(icmp->f, V6, np->target, np->lnaddr, 8*np->olen-2, np->target, ifc, 0);
		else if(ipv6local(ifc, ia, 0, np->target))
			arpenter(icmp->f, V6, np->target, np->lnaddr, 8*np->olen-2, ia, ifc, 1);
		freeblist(bp);
		break;

	default:
		goticmpkt6(icmp, bp, 0);
		break;
	}
	return;
raise:
	freeblist(bp);
}

static int
icmpstats6(Proto *icmp6, char *buf, int len)
{
	Icmppriv6 *priv;
	char *p, *e;
	int i;

	priv = icmp6->priv;
	p = buf;
	e = p+len;
	for(i = 0; i < Nstats6; i++)
		p = seprint(p, e, "%s: %lud\n", statnames6[i], priv->stats[i]);
	for(i = 0; i <= Maxtype6; i++)
		if(icmpnames6[i])
			p = seprint(p, e, "%s: %lud %lud\n", icmpnames6[i],
				priv->in[i], priv->out[i]);
	return p - buf;
}


/* import from icmp.c */
extern int	icmpstate(Conv *c, char *state, int n);
extern char*	icmpannounce(Conv *c, char **argv, int argc);
extern char*	icmpconnect(Conv *c, char **argv, int argc);
extern void	icmpclose(Conv *c);

static void
icmpclose6(Conv *c)
{
	Icmpcb6 *icb = (Icmpcb6*)c->ptcl;
	icb->headers = 0;
	icmpclose(c);
}

void
icmp6init(Fs *fs)
{
	Proto *icmp6 = smalloc(sizeof(Proto));

	icmp6->priv = smalloc(sizeof(Icmppriv6));
	icmp6->name = "icmpv6";
	icmp6->connect = icmpconnect;
	icmp6->announce = icmpannounce;
	icmp6->state = icmpstate;
	icmp6->create = icmpcreate6;
	icmp6->close = icmpclose6;
	icmp6->rcv = icmpiput6;
	icmp6->stats = icmpstats6;
	icmp6->ctl = icmpctl6;
	icmp6->advise = icmpadvise6;
	icmp6->gc = nil;
	icmp6->ipproto = ICMPv6;
	icmp6->nc = 16;
	icmp6->ptclsize = sizeof(Icmpcb6);

	Fsproto(fs, icmp6);
}
