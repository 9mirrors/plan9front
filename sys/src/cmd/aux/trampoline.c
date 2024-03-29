#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ndb.h>
#include <fcall.h>

enum
{
	Maxpath=	128,
};

typedef struct Endpoints Endpoints;
struct Endpoints
{
	char 	*net;
	char	*lsys;
	char	*lserv;
	char	*rsys;
	char	*rserv;
};

void		xfer(int, int);
void		xfer9p(int, int);
Endpoints*	getendpoints(char*);
char*		iptomac(char*, char*);
int		macok(char*);

int		timeout;
int		activity;

int		nopts;
char		*opts[16];

void
usage(void)
{
	fprint(2, "usage: trampoline [-9] [-a addr] [-m netdir] [-o opt] [-t timeout] addr\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *altaddr, *checkmac, *mac;
	int fd, fd0, fd1, ctl, i;
	void (*x)(int, int);
	Endpoints *ep;

	checkmac = nil;
	altaddr = nil;
	x = xfer;
	ARGBEGIN{
	case '9':
		x = xfer9p;
		break;
	case 'a':
		altaddr = EARGF(usage());
		break;
	case 'm':
		checkmac = EARGF(usage());
		break;
	case 't':
		timeout = atoi(EARGF(usage()));
		break;
	case 'o':
		if(nopts >= nelem(opts))
			sysfatal("too many -o options");
		opts[nopts++] = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(argc != 1)
		usage();

	if(checkmac){
		ep = getendpoints(checkmac);
		mac = iptomac(ep->rsys, ep->net);
		if(!macok(mac)){
			syslog(0, "trampoline", "badmac %s from %s!%s for %s!%s on %s",
				mac, ep->rsys, ep->rserv, ep->lsys, ep->lserv, ep->net);
			exits("bad mac");
		}
	}
	
	if(timeout > 0)
		alarm(timeout);

	fd0 = 0;
	fd1 = 1;
	if(altaddr){
		fd0 = dial(altaddr, 0, 0, 0);
		if(fd0 < 0)
			sysfatal("dial %s: %r", altaddr);
		fd1 = fd0;
	}
	fd = dial(argv[0], 0, 0, &ctl);
	if(fd < 0)
		sysfatal("dial %s: %r", argv[0]);

	for(i = 0; i < nopts; i++){
		if(write(ctl, opts[i], strlen(opts[i])) < 0)
			fprint(2, "%s: can't write %s: %r\n", argv0, opts[i]);
	}
	close(ctl);

	rfork(RFNOTEG);
	if(timeout > 0){
		alarm(0);
		switch(rfork(RFPROC|RFMEM)){
		case -1:
			fprint(2, "%s: fork: %r\n", argv0);
			exits("fork");
		case 0:
			break;
		default:
			do {
				activity = 0;
				sleep(timeout);
			} while(activity);
			postnote(PNGROUP, getpid(), "timeout");
			exits("timeout");
		}
	}
	switch(rfork(RFPROC|RFMEM)){
	case -1:
		fprint(2, "%s: fork: %r\n", argv0);
		if(timeout <= 0) exits("fork");
		break;
	case 0:
		(*x)(fd0, fd);
		break;
	default:
		(*x)(fd, fd1);
		break;
	}
	postnote(PNGROUP, getpid(), "hangup");
	exits(nil);
}

void
xfer(int from, int to)
{
	char buf[12*1024];
	int n;

	while((n = read(from, buf, sizeof buf)) > 0){
		if(write(to, buf, n) < 0)
			break;
		if(timeout > 0)
			activity = 1;
	}
}

void
xfer9p(int from, int to)
{
	uchar *buf;
	uint nbuf;
	int n;

	nbuf = 256;
	buf = malloc(nbuf);
	if(buf == nil)
		sysfatal("xfer: malloc %ud: %r", nbuf);

	for(;;){
		if(readn(from, buf, 4) != 4)
			break;
		n = GBIT32(buf);
		if(n > nbuf){
			nbuf = n+8192;
			buf = realloc(buf, nbuf);
			if(buf == nil)
				sysfatal("xfer: realloc %ud: %r", nbuf);
		}
		if(readn(from, buf+4, n-4) != n-4)
			break;
		if(write(to, buf, n) != n)
			break;
		if(timeout > 0)
			activity = 1;
	}
}

void
getendpoint(char *dir, char *file, char **sysp, char **servp)
{
	int fd, n;
	char buf[Maxpath];
	char *sys, *serv;

	sys = serv = 0;

	snprint(buf, sizeof buf, "%s/%s", dir, file);
	fd = open(buf, OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof(buf)-1);
		if(n>0){
			buf[n-1] = 0;
			serv = strchr(buf, '!');
			if(serv){
				*serv++ = 0;
				serv = strdup(serv);
			}
			sys = strdup(buf);
		}
		close(fd);
	}
	if(serv == 0)
		serv = strdup("unknown");
	if(sys == 0)
		sys = strdup("unknown");
	*servp = serv;
	*sysp = sys;
}

Endpoints *
getendpoints(char *dir)
{
	Endpoints *ep;
	char *p;

	ep = malloc(sizeof(*ep));
	ep->net = strdup(dir);
	p = strchr(ep->net+1, '/');
	if(p == nil){
		free(ep->net);
		ep->net = "/net";
	} else
		*p = 0;
	getendpoint(dir, "local", &ep->lsys, &ep->lserv);
	getendpoint(dir, "remote", &ep->rsys, &ep->rserv);
	return ep;
}

char*
iptomac(char *ip, char *net)
{
	char file[Maxpath];
	Biobuf *b;
	char *p;
	char *f[5];

	snprint(file, sizeof(file), "%s/arp", net);
	b = Bopen(file, OREAD);
	if(b == nil)
		return nil;
	while((p = Brdline(b, '\n')) != nil){
		p[Blinelen(b)-1] = 0;
		if(tokenize(p, f, nelem(f)) < 4)
			continue;
		if(strcmp(f[1], "OK") == 0
		&& strcmp(f[2], ip) == 0){
			p = strdup(f[3]);
			Bterm(b);
			return p;
		}
	}
	Bterm(b);
	return nil;
}

int
macok(char *mac)
{
	char *p;

	if(mac == nil)
		return 0;
	free(p = csgetvalue("/net", "ether", mac, "trampok", nil));
	return p != nil;
}
