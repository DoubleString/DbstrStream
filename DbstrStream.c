/*
 * DbstrStream.c
 *
 *  Created on: 2016年9月21日
 *      Author: doublestring
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "list.h"

#define socket_t int
#define epoll_t int
#define closesocket close

#define STREAM_TCP 1

#define MODE_TCPSVR (STREAM_TCP+1)
#define MODE_TCPCLI (MODE_TCPSVR+1)

#define MODE_NTRIPCLI (MODE_TCPCLI+1)
#define MODE_NTRIPSVR (MODE_NTRIPCLI+1)

#define payloadsize 1024
#define maxstrsize 256
#define floor(f) ((int)(f>(int)f ? f+1:f))
#ifdef WIN32

#else
#define lock_t pthread_mutex_t
#define lock(f) pthread_mutex_lock(f)
#define trylock(f) pthread_mutex_trylock(f)
#define unlock(f) pthread_mutex_unlock(f)
#define initlock(f) pthread_mutex_init(f,NULL)
#endif
static int ticonnect = 10000;

static char ICY_OK[]="ICY 200 OK";
static char ICY_UN[]="Unauthorized";
static char ICY_SOURCE[]="SOURCETABLE";


#define MAXEPOLL 1024
typedef void (*dataRecvCallback)(char *buff, int n, void* arg);

typedef struct {
	int state; /* 0:close 1:wait 2:connect*/
	char saddr[256];
	int port;
	struct sockaddr_in addr;
	socket_t sock;

	unsigned int tact; /*data active tick*/
	unsigned int tdis; /*disconnect tick*/
} tcp_t;
typedef struct {
	struct list_head list;
	char buff[payloadsize];
	unsigned int n;
} packet_list;

typedef struct {
	tcp_t cli;
	int inactinv; /*inactive timeout (ms) (0:no timeout)*/
	int reconinv; /*reconnect time interval (ms) (0:no reconnect)*/
	packet_list sndpkt;
	lock_t synccli;/*lock for packet queue*/
	int snredy; /*flag for whether the send buffer is ready*/

	dataRecvCallback callback; /*callback for client mode*/

	void* host;
	pthread_t pth_recon;
} tcpcli_t;

typedef struct {
	struct list_head list;
	tcpcli_t *client;
} tcpcli_list;

typedef struct {
	tcp_t svr;
	tcpcli_list clients;
	dataRecvCallback callback;

	lock_t syncsvr;
} tcpsvr_t;
typedef struct {
	tcpcli_t *cli;
	char psd[maxstrsize];
	char usr[maxstrsize];
	char mountpoint[maxstrsize];
	int state; /*0:close 1:wait 2:connect*/
	void* host;
} ntrip_t;

typedef struct {
	struct list_head list;
	ntrip_t ntrcli;
} ntrip_list;
typedef struct {
	int type;
	int mode;
	epoll_t efd;
	union {
		tcpcli_list *cliHead;
		tcpsvr_t *svr;
		ntrip_list *ntrHead;
	} prot;
	lock_t synlock;
} stream_t;

#ifdef WIN32

#else
static int errsock(void) {
	return errno;
}

#endif
/* get tick time ---------------------------------------------------------------
 * get current tick in ms
 * args   : none
 * return : current tick in ms
 *-----------------------------------------------------------------------------*/
extern unsigned int tickget(void) {
#ifdef WIN32
	return (unsigned int)timeGetTime();
#else
	struct timespec tp = { 0 };
	struct timeval tv = { 0 };

#ifdef CLOCK_MONOTONIC_RAW
	/* linux kernel > 2.6.28 */
	if (!clock_gettime(CLOCK_MONOTONIC_RAW, &tp)) {
		return tp.tv_sec * 1000u + tp.tv_nsec / 1000000u;
	} else {
		gettimeofday(&tv, NULL);
		return tv.tv_sec * 1000u + tv.tv_usec / 1000u;
	}
#else
	gettimeofday(&tv,NULL);
	return tv.tv_sec*1000u+tv.tv_usec/1000u;
#endif
#endif /* WIN32 */
}

static int setsock(socket_t sock) {
	int opts;
	if ((opts = fcntl(sock, F_GETFL)) < 0) {
		printf("fcntl(F_GETFL) error!\n");
		return 0;
	}
	opts = (opts | O_NONBLOCK);

	if (fcntl(sock, F_SETFL, opts) < 0) {
		printf("fcntl(F_SETFL) error!\n");
		return 0;
	}

	return 1;
}
static int gentcp(tcp_t *tcp, int type) {
	struct hostent *hp;
	/* generate socket */
	if ((tcp->sock = socket(AF_INET, SOCK_STREAM, 0)) == (socket_t) -1) {
		printf("socket error (%d)", errsock());
		tcp->state = -1;
		return 0;
	}

	if (!setsock(tcp->sock)) {
		printf("socket error (%d)", errsock());
		tcp->state = -1;
		return 0;
	}
	memset(&tcp->addr, 0, sizeof(struct sockaddr_in));
	tcp->addr.sin_family = AF_INET;
	tcp->addr.sin_port = htons(tcp->port);

	/*server type*/
	if (type == 0) {
		if (bind(tcp->sock, (struct sockaddr *) &tcp->addr, sizeof(tcp->addr))
				== -1) {
			printf("bind error (%d) : %d", errsock(), tcp->port);
			closesocket(tcp->sock);
			tcp->state = -1;
			return 0;
		}
		listen(tcp->sock, 10);
	} else {
		if (!(hp = gethostbyname(tcp->saddr))) {
			printf("address error (%s)", tcp->saddr);
			closesocket(tcp->sock);
			tcp->state = 0;
			tcp->tdis = tickget();
			return 0;
		}
//    	inet_aton(tcp->saddr,&tcp->addr.sin_addr);
		memcpy(&(tcp->addr.sin_addr), hp->h_addr, hp->h_length);
	}

	tcp->state = 1;
	tcp->tact = tickget();

	return 1;
}

static tcpcli_t* opentcpcli(char *addr,int port) {
	tcpcli_t* tcpcli, tcpcli0 = { { 0 } };
	if (!(tcpcli = (tcpcli_t*) malloc(sizeof(tcpcli_t)))) {
		printf("malloc error!\n");
		return NULL;
	}
	*tcpcli = tcpcli0;
	strcpy(tcpcli->cli.saddr,addr);
	tcpcli->cli.port=port;
	if (!gentcp(&tcpcli->cli, 1)) {
		free(tcpcli);
		tcpcli = NULL;
		return NULL;
	}

	tcpcli->inactinv = 1000 * 3600;
	tcpcli->reconinv = 1000;
	tcpcli->snredy = 1;
	tcpcli->pth_recon = -1;
	/*init lock*/
	initlock(&tcpcli->synccli);
	/*init queue*/
	list_init(&tcpcli->sndpkt.list);

	return tcpcli;
}

static int connect_nb(tcp_t* tcp) {
	errno = 0;
	tcp->state = 0;
	int rc = connect(tcp->sock, (struct sockaddr*) &tcp->addr,
			sizeof(tcp->addr));
	if (rc == -1) {
		if (errno == EINPROGRESS) {
			tcp->state = 1;
			return 1;
		}
	} else if (rc == 0) {
		tcp->state = 2;
		printf(
				"no-blocking connect success! connect to the svr immediately!\n");
		return 2;
	}
	return 0;
}

static tcpsvr_t* opentcpsvr(char *addr, int port) {
	tcpsvr_t *tcpsvr, tcpsvr0 = { { 0 } };
	if ((tcpsvr = (tcpsvr_t*) malloc(sizeof(tcpsvr_t))) == NULL) {
		printf("malloc error\n");
		return NULL;
	}
	*tcpsvr = tcpsvr0;
	if (!gentcp(&tcpsvr->svr, 0)) {
		free(tcpsvr);
		tcpsvr = NULL;
		return NULL;
	}
	initlock(&tcpsvr->syncsvr);
	list_init(&(tcpsvr->clients.list));
	return tcpsvr;
}

static tcpcli_t* gettcpcli(tcpsvr_t *svr, int fd) {
	tcpcli_list *poscli;
	struct list_head* head = &(svr->clients.list);
	list_for_each_entry(poscli,head,list)
	{
		if (poscli->client->cli.sock == fd) {
			return poscli->client;
		}
	}
	return NULL;
}

static int accept_nb(stream_t* stream) {
	struct sockaddr_in addr;
	struct epoll_event ev;
	socklen_t len = sizeof(struct sockaddr);
	tcpsvr_t* svr = stream->prot.svr;

	/*if it is ET mode ,here should be a loop*/
	while (1) {

		int fd = accept(svr->svr.sock, (struct sockaddr*) &addr, &len);

		if (fd == -1 && errno == EWOULDBLOCK)
			break;

		if (!setsock(fd))
			return 0;

		ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
		ev.data.fd = fd;

		if (epoll_ctl(stream->efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
			perror("epoll_ctl failed! server break down!\n");
			exit(1);
		}

		/*add client information*/
		tcpcli_t *cli, cli0 = { { 0 } };
		cli = (tcpcli_t*) malloc(sizeof(tcpcli_t));
		*cli = cli0;

		cli->snredy = 1;
		initlock(&(cli->synccli));
		cli->reconinv = 0;
		cli->inactinv = 1000 * 7200; /*two hour for inactive socket*/
		list_init(&cli->sndpkt.list);

		/*update socket configure*/
		cli->cli.sock = fd;
		cli->cli.state = 2;

		cli->cli.tact = tickget();
		memcpy(&(cli->cli.addr), &addr, sizeof(addr));
		strcpy(cli->cli.saddr, inet_ntoa(addr.sin_addr));

		/*add tcpclilist*/
		tcpcli_list* tcpcli = (tcpcli_list*) malloc(sizeof(tcpcli_list));
		tcpcli->client = cli;
		list_add_tail(&tcpcli->list, &(svr->clients.list));
	}

	return 1;
}
static int send_packet(tcpcli_t *cli) {
	lock(&(cli->synccli));
	packet_list* pos = NULL;
	int nsend, nrem, sumsd;
	char buff[payloadsize];
	while (!list_empty(&cli->sndpkt.list)) {
		pos = list_entry(cli->sndpkt.list.next, typeof(*pos), list);
		nrem = pos->n;
		sumsd = 0;
		while (nrem > 0) {
			/*send the whole packet*/
			nsend = send(cli->cli.sock, pos->buff + sumsd, nrem, 0);
			if (nsend == -1
					&& (errno == EAGAIN || errno == EWOULDBLOCK
							|| errno == EINTR)) {
				/*update the remain packet */
				memcpy(buff, pos->buff, pos->n);
				memset(pos->buff, 0, sizeof(pos->buff));
				memcpy(pos->buff, buff + sumsd, nrem);
				pos->n = nrem;
				break;
			}
			sumsd += nsend;
			nrem -= sumsd;
		}

		if (nrem > 0) {
			break;
		}
		/*free memory*/
		list_del(&pos->list);
		free(pos);
		pos = NULL;
	}

	if (list_empty(&cli->sndpkt.list)) {
		//should open
		cli->snredy = 1;
		/*change epoll model mode*/
	}
	unlock(&(cli->synccli));
}

/*to get the tcpcli_t,should lock the svr.syncsvr and loop the linked list.
 so the bug of when the client memory is freed,the following code wouldn't cause
 segmentation fault********************************************************/
static int send_nb(tcpcli_t* cli, char* buff, int len) {
	int i, sumsd, nrem, nsend, num;

	packet_list *payload;

	lock(&(cli->synccli));

	if (cli->snredy) {
		nrem = len;
		sumsd = 0;
		while (nrem > 0) {
			nsend = send(cli->cli.sock, buff + sumsd, nrem, 0);
			if (nsend == -1
					&& (errno == EAGAIN || errno == EWOULDBLOCK
							|| errno == EINTR)) {
				/*write buffer is full*/
				break;
			}
			sumsd += nsend;
			nrem -= sumsd;
		}
		if (nrem > 0) {
			/*update the remain packet */
			num = floor((nrem+0.0) / payloadsize);
			for (i = 0; i < num; i++) {
				payload = (packet_list*) malloc(sizeof(packet_list));

				memcpy(payload->buff, buff + sumsd,
						i == num - 1 ? nrem - i * payloadsize : payloadsize);
				payload->n =
						i == num - 1 ? nrem - i * payloadsize : payloadsize;

				list_add_tail(&payload->list, &cli->sndpkt.list);
			}
			cli->snredy = 0;
		}
	} else {
		/*add the buffer packet */

		num = floor((len+0.0) / payloadsize);
		for (i = 0; i < num; i++) {
			payload = (packet_list*) malloc(sizeof(packet_list));
			memcpy(payload->buff, buff + i * payloadsize,
					i == num - 1 ? len - i * payloadsize : payloadsize);
			payload->n = i == num - 1 ? len - i * payloadsize : payloadsize;
			list_add_tail(&payload->list, &cli->sndpkt.list);
		}
	}
	unlock(&(cli->synccli));
	return 1;
}
static void freetcpcli(tcpcli_t *cli) {

	/*free packet memory
	 * there is no need to lock the synccli anymore because the syncsvr is locked
	 * */

//	lock(&(cli->synccli));
	packet_list *pos = NULL;
	while (!list_empty(&cli->sndpkt.list)) {
		pos = list_entry(cli->sndpkt.list.next, typeof(*pos), list);
		list_del(&pos->list);
		free(pos);
		pos = NULL;
	}
	/*free tcpcli_t memory*/
//	unlock(&(cli->synccli));
	free(cli);
	cli = NULL;
}

static int updatesvr(stream_t *stream, int fd) {

	tcpsvr_t *svr = stream->prot.svr;

	tcpcli_list *cliHead = &svr->clients;
	tcpcli_list *recu = NULL, *tmp;
	struct epoll_event ev;

	recu = list_entry(cliHead->list.next, typeof(*recu), list);
	while (&recu->list != &cliHead->list) {
		/*update closed socket*/
		tmp = recu->client->cli.state == 0 ? recu : NULL;

		/*update socket fd */
		if (tmp == NULL)
			tmp = recu->client->cli.sock == fd ? recu : NULL;
		/*update timeout socket*/
		if (tmp == NULL) {

		}
		recu = list_entry(recu->list.next, typeof(*recu), list);

		if (tmp) {
			close(tmp->client->cli.sock); /*if all the reference to the socket have been closed,the epoll model will automatically remove it from the efd*/
			/*free memory*/

			lock(&svr->syncsvr);

			list_del(&tmp->list);

			freetcpcli(tmp->client);
			tmp = NULL;

			unlock(&svr->syncsvr);

		}
	}
	return 1;
}
extern int sendbuff(tcpsvr_t *svr, char *buffer, int n) {
	lock(&svr->syncsvr);

	unlock(&svr->syncsvr);
}
static int disconcli(tcpsvr_t *svr, int fd) {
	tcpcli_t *cli = gettcpcli(svr, fd);
	if (cli != NULL) {
		cli->cli.state = 0;
		cli->cli.tdis = tickget();
		return 1;
	}
	return 0;
}

static int starttcpsvr() {
	stream_t stream;
	tcpsvr_t* svr;
	tcpcli_t* cli;
	char buffer[payloadsize];
	int nfd, i, curfd, nread;

	if (!(svr = opentcpsvr("127.0.0.1", 8000))) {
		return 0;
	}
	stream.prot.svr = svr;

	struct epoll_event events[MAXEPOLL], ev;
	stream.mode = MODE_TCPSVR;
	stream.efd = epoll_create(MAXEPOLL);

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = svr->svr.sock;

	epoll_ctl(stream.efd, EPOLL_CTL_ADD, svr->svr.sock, &ev);

	while (1) {
		nfd = epoll_wait(stream.efd, events, MAXEPOLL, -1); /*infinite wait until it ready*/
		for (i = 0; i < nfd; i++) {

			curfd = events[i].data.fd;
			if (curfd == svr->svr.sock && (events[i].events & EPOLLIN)) {
				/*accept function finished*/
				accept_nb(&stream);
			} else if (events[i].events & EPOLLIN) {
				/*receiving function finished*/
				while (1) {
					nread = recv(events[i].data.fd, buffer, sizeof(buffer), 0);
					if (nread < 0
							&& ( errno == EINTR || errno == EWOULDBLOCK
									|| errno == EAGAIN)) {
						break;
					} else if (nread <= 0) {
						updatesvr(&stream, events[i].data.fd);
						break;
					}
					if (svr->callback != NULL) {
						svr->callback(buffer, nread, curfd);
					}
				}
			}
			if (events[i].events & EPOLLOUT) {
				/*sending model under constructed*/
				cli = gettcpcli(svr, curfd); /*can be added from the events[i].data.ptr*/
				send_packet(cli);
			}
		}
	}
	return 1;
}

static int testcon_cli(socket_t fd) {
	int status, err;
	int len = sizeof(int);
	status = getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*) &err,
			(socklen_t*) &len);
	if (status < 0) {
		perror("getsocket error!\n");
		return 1;
	}
	if (err != 0) {
		perror("connect error!\n");
		return 0;
	}
	printf("no-blocking connect success!\n");
	return 2;
}
static int discont_cli(stream_t* stream, tcpcli_t* cli) {
	lock(&stream->synlock);

	unlock(&stream->synlock);
	return 1;

}

static void openntrip(ntrip_t* ptrntr){
	char cmd[maxstrsize],*base64,req[maxstrsize];
	sprintf(cmd,"%s:%s",ptrntr->usr,ptrntr->psd);
	base64=base64_encode(cmd,strlen(cmd));
	sprintf(req,"GET /%s HTTP/1.0\r\nUser-Agent: NTRIP GNSSInternetRadio/1.2.0\r\nAuthorization: Basic %s\r\n\r\n",
			ptrntr->mountpoint,base64);
	send_nb(ptrntr->cli,req,strlen(req));
}
void recon_callback(void* arg) {
	tcpcli_t* ptrcli = (tcpcli_t*) arg;
	ntrip_t* ntrcli = (ntrip_t*) ptrcli->host;

	stream_t* stream=ntrcli->host;

	struct epoll_event ev;
	if (!gentcp(&ptrcli->cli, 1)) {
		/*disconnect cli*/
		printf("disconnect client %d!\n", ptrcli->cli.sock);
		ntrcli->state=0;
		ptrcli->cli.state=0;
		ptrcli->pth_recon=-1;
		return;
	}
	do {
		if (ptrcli->reconinv != 0) {
			usleep(ptrcli->reconinv * 1000);
			printf("enter reconnect process\n");
		} else
			usleep(100 * 1000);
	} while (!(ntrcli->state=connect_nb(&ptrcli->cli)));
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	switch(stream->mode){
		case MODE_TCPCLI:
		case MODE_TCPSVR:   ev.data.ptr = (void*) ptrcli; break;
		case MODE_NTRIPCLI:
		case MODE_NTRIPSVR: ev.data.ptr = (void*) ntrcli;   break;
	}
	epoll_ctl(stream->efd, EPOLL_CTL_ADD, ptrcli->cli.sock, &ev);
	/*in case of connecting immediately*/
	if(ntrcli->state==2){
		openntrip(ntrcli);
	}
	ptrcli->pth_recon = -1;
	printf("connect process success!\n");
}
static int recon_cli(tcpcli_t* cli) {
	pthread_t pth_t;
	if (cli->pth_recon == -1) {
		cli->cli.state = 1;
		if (-1
				== pthread_create(&cli->pth_recon, NULL, recon_callback,
						(void*) cli)) {
			printf("thread for reconnect failed!\n");
			cli->pth_recon = -1;
			cli->cli.state = 0;
			((ntrip_t*)cli->host)->state=0;
			/*create thread failed remove the client*/
//			discont_cli((stream_t*)((ntrip_t*) cli->host)->host, cli);
			return 0;
		}
	}
}
static int shutdowntcp_cli(stream_t* stream) {

}

static void send_cli(stream_t *stream, char* buffer, int n) {
	/*lock in case of free tcpcli*/
	lock(&stream->synlock);
	tcpcli_list *cliHead = stream->prot.cliHead;
	tcpcli_list *pos = NULL;
	list_for_each_entry(pos,&cliHead->list,list)
	{
		if (pos->client->cli.state == 2) {
			send_nb(pos->client, buffer, n);
		}
	}
	unlock(&stream->synlock);
}

static int starttcpcli(stream_t *stream, dataRecvCallback callback, int ncli) {
	tcpcli_t *cli, *ptrcli;
	int nfd, i, fd, nread, ret;
	char buffer[payloadsize];
	struct epoll_event events[MAXEPOLL], ev;
	/********************************INIT-CONFIG***********************************************/
	stream->mode = MODE_TCPCLI;
	stream->type = STREAM_TCP;
	stream->efd = epoll_create(MAXEPOLL);
	stream->prot.cliHead = (tcpcli_list*) malloc(sizeof(tcpcli_list));
	list_init(&stream->prot.cliHead->list);
	initlock(&stream->synlock);
	/*********************************READ-CONFIG-FILE*****************************************/

	/******************************************************************************************/
	lock(&stream->synlock); /*lock because reconnect thread may change the stream.prot.cliHead*/
	for (i = 0; i < ncli; i++) {
		if (!(cli = opentcpcli("127.0.0.1",8002))) {
			printf("failed to opencli!\n");
			continue;
		}
		cli->host = (void*) stream;
		printf("opencli successfully! fd id:%d \n", cli->cli.sock);
		if (callback != NULL)
			cli->callback = callback;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		ev.data.ptr = (void*) cli;
		epoll_ctl(stream->efd, EPOLL_CTL_ADD, cli->cli.sock, &ev);

		/*add to the stream*/
		tcpcli_list* tcpcli = (tcpcli_list*) malloc(sizeof(tcpcli_list));
		tcpcli->client = cli;
		list_add_tail(&tcpcli->list, &stream->prot.cliHead->list);

		if (!connect_nb(&cli->cli)) {
			printf("sock %d connect failed! now trying to reconnect!\n",
					cli->cli.sock);
			epoll_ctl(stream->efd, EPOLL_CTL_DEL, cli->cli.sock, NULL);
			recon_cli(cli);
			continue;
		}
	}
	unlock(&stream->synlock);

	while (1) {
		nfd = epoll_wait(stream->efd, events, MAXEPOLL, -1); /*infinite wait until it is ready*/
		for (i = 0; i < nfd; i++) {
			ptrcli = (tcpcli_t*) events[i].data.ptr;
			fd = ptrcli->cli.sock;
			if (events[i].events & EPOLLERR) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2)
						ptrcli->cli.state = 2;
					if (ret < 2)
						break;
				}
			}
			if (events[i].events & EPOLLIN) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2)
						ptrcli->cli.state = 2;
					if (ret < 2)
						break;
				}
				while (1) {
					nread = recv(fd, buffer, sizeof(buffer), 0);
					if (nread < 0
							&& ( errno == EINTR || errno == EWOULDBLOCK
									|| errno == EAGAIN)) {
						break;
					} else if (nread <= 0) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						ptrcli->cli.state = 0;
						recon_cli(ptrcli);
						break;
					}
					if (ptrcli->callback != NULL) {
						ptrcli->callback(buffer, nread, &fd);
					}
				}
			}
			if (events[i].events & EPOLLOUT) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2)
						ptrcli->cli.state = 2;
					if (ret < 2)
						break;
				}
				send_packet(ptrcli);
			}
		}
	}
	return 1;
}
/************************************************TEST FOR NO-BLOCKING TCP CLIENT*****************************************************************************/
static void callback(char* buffer, int n, void* arg) {
#define maxfd 20

	int fd = *(int*) arg;
	static FILE* fp[maxfd];
	static int fdarray[maxfd] = { 0 };
	int i;
	char filename[64];
	for (i = 0; i < maxfd; i++) {
		if (fd == fdarray[i] || fdarray[i] == 0)
			break;
	}
	if (fdarray[i] == 0) {
		sprintf(filename, "/home/doublestring/workspace/tcp-test-folder/%s_%d",
				"file_recv", fd);
		if (!(fp[i] = fopen(filename, "aw+"))) {
			printf("cant open file to write!\n");
			exit(1);
		}
		fdarray[i] = fd;
	}
	fwrite(buffer, sizeof(char), n, fp[i]);
	fflush(fp[i]);
}
void pth_recv(void* arg) {
	int ncli = 1;
	stream_t *stream = (stream_t*) arg;
	starttcpcli(stream, callback, ncli);
}

int ma_in(int argc, char *args[]) {
	pthread_t pth_t;
	stream_t stream;
	FILE *fp;
	char buffer[payloadsize * 5];
	int len;
	if (-1 == pthread_create(&pth_t, NULL, pth_recv, &stream)) {
		perror("cant create thread for cli!\n");
		exit(1);
	}
	FILE* savfile = fopen("/home/doublestring/savfile", "w");
	while (1) {
		printf("Please input the filename:\n");
		scanf("%s", buffer);
		if (!(fp = fopen(buffer, "r"))) {
			printf("cant open file %s to read!\n", buffer);
			continue;
		}
		printf("enter to continue");
		getchar();
		while (!feof(fp)) {
			len = fread(buffer, sizeof(char), payloadsize * 5, fp);
			send_cli(&stream, buffer, len);
			fwrite(buffer, sizeof(char), len, savfile);
			fflush(savfile);
		}
		fclose(fp);
	}
	fclose(savfile);
}

/*********************************************************************************************************************************************************/




/**************************************************************MULTI-NTRIP-CLIENT*************************************************************************/
int modified_julday(int iyear, int imonth, int iday) {
	int iyr, result;
	int doy_of_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304,
			334 };
	if (iyear < 0 || imonth < 0 || iday < 0 || imonth > 12 || iday > 366
			|| (imonth != 0 && iday > 31)) {
		printf("ERROR(modified_julday)incorrect input arguments!%d %d %d\n",
				iyear, imonth, iday);
		exit(1);
	}
	iyr = iyear;
	if (imonth <= 2)
		iyr -= 1;
	result = 365 * iyear - 678941 + iyr / 4 - iyr / 100 + iyr / 400 + iday;

	if (imonth != 0)
		result = result + doy_of_month[imonth - 1];

	return result;

}
void mjd2doy(int jd, int* iyear, int* idoy) {
	*iyear = (jd + 678940) / 365;
	*idoy = jd - modified_julday(*iyear, 1, 1);
	while (*idoy < 0) {
		(*iyear)--;
		*idoy = jd - modified_julday(*iyear, 1, 1) + 1;
	}

}
int run_tim() {
	struct tm *ptr;
	time_t rawtime;
	time(&rawtime);
	ptr = localtime(&rawtime);
	int iyear,idoy;
	int mjd=modified_julday(ptr->tm_year+1900,ptr->tm_mon+1,ptr->tm_mday);
	mjd2doy(mjd,&iyear,&idoy);
	return idoy;
}
static void ntrrecv_callback(char* buffer,int n,void* arg){
	ntrip_t* ptrntr = (ntrip_t*) arg;
	static FILE*fp=NULL;
	static int id;
	if(ptrntr->state<=0)
		return;
	if(ptrntr->state==1){
		printf("%s",buffer);
		if (strstr(buffer, ICY_OK) != NULL){
			ptrntr->state=2;
		}
		else if (strstr(buffer, ICY_UN) != NULL) {
			printf("登录信息错误,请检查用户名,密码,挂载点是否输入正确!\n");

		} else if (strstr(buffer, ICY_SOURCE) != NULL) {
			printf("请选择挂载点!");
		}
		return;
	}

	int iday=run_tim();
	char buff[1024];
	if (id != iday) {
		if(fp)
			fclose(fp);
		sprintf(buff,"savfile_%s_%d",ptrntr->mountpoint,iday);
		if(!(fp=fopen(buff,"w"))){
			printf("cant open file to write :%s \n",buff);
			exit(1);
		}
		id=iday;
	}

	fwrite(buffer,sizeof(char),n,fp);
	fflush(fp);
}
static int startntripcli(stream_t *stream, dataRecvCallback callback, int ncli) {
	tcpcli_t *cli,*ptrcli;
	ntrip_t *ptrntr,ptrntr0={{0}};
	int nfd, i, fd, nread, ret;
	char buffer[payloadsize];
	struct epoll_event events[MAXEPOLL], ev;
	/********************************INIT-CONFIG***********************************************/
	stream->mode = MODE_NTRIPCLI;
	stream->type = STREAM_TCP;
	stream->efd = epoll_create(MAXEPOLL);

	stream->prot.ntrHead=(ntrip_list*)malloc(sizeof(ntrip_list));
	list_init(&stream->prot.ntrHead->list);
	initlock(&stream->synlock);
	/*********************************READ-CONFIG-FILE*****************************************/

	/******************************************************************************************/
	lock(&stream->synlock); /*lock because reconnect thread may change the stream.prot.cliHead*/
	for (i = 0; i < ncli; i++) {
		if (!(cli = opentcpcli("59.175.223.165",2101))) {
			printf("failed to opencli!\n");
			continue;
		}

		printf("opencli successfully! fd id:%d \n", cli->cli.sock);
		if (callback != NULL)
			cli->callback = callback;

		/*add to the stream*/
		ntrip_list* ntrlist = (ntrip_list*)malloc(sizeof(ntrip_list));
		ntrlist->ntrcli=ptrntr0;

		/*add for test*/
		strcpy(ntrlist->ntrcli.mountpoint,"QLZ1");
		strcpy(ntrlist->ntrcli.usr,"dd");
		strcpy(ntrlist->ntrcli.psd,"111111");
		strcpy(cli->cli.saddr,"58.49.58.149");
		cli->cli.port=2101;




		ntrlist->ntrcli.cli=cli;
		ntrlist->ntrcli.host=(void*)stream;
		list_add_tail(&ntrlist->list,&stream->prot.ntrHead->list);
		cli->host = (void*)&ntrlist->ntrcli;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		ev.data.ptr = (void*)&ntrlist->ntrcli;
		epoll_ctl(stream->efd, EPOLL_CTL_ADD, cli->cli.sock, &ev);

		if (!(ntrlist->ntrcli.state=connect_nb(&cli->cli))) {
			printf("sock %d connect failed! now trying to reconnect!\n",
					cli->cli.sock);
			epoll_ctl(stream->efd, EPOLL_CTL_DEL, cli->cli.sock, NULL);
			recon_cli(cli);
			continue;
		}
		if(ntrlist->ntrcli.state==2){
			openntrip(&ntrlist->ntrcli);
		}
	}
	unlock(&stream->synlock);

	while (1) {
		nfd = epoll_wait(stream->efd, events, MAXEPOLL, -1); /*infinite wait until it is ready*/
		for (i = 0; i < nfd; i++) {
			ptrntr = (ntrip_t*)events[i].data.ptr;
			ptrcli=ptrntr->cli;
			fd = ptrcli->cli.sock;
			if (events[i].events & EPOLLERR) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2){
						ptrcli->cli.state = 2;
						ptrntr->state=1;
						/*open ntrip config*/
						openntrip(ptrntr);
					}
					if (ret < 2)
						break;
				}
			}
			if (events[i].events & EPOLLIN) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2){
						ptrcli->cli.state = 2;
						ptrntr->state=1;
						/*open ntrip config*/
						openntrip(ptrntr);
					}
					if (ret < 2)
						break;
				}
				while (1) {
					nread = recv(fd, buffer, sizeof(buffer), 0);
					if (nread < 0
							&& ( errno == EINTR || errno == EWOULDBLOCK
									|| errno == EAGAIN)) {
						break;
					} else if (nread <= 0) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						ptrcli->cli.state = 0;
						ptrntr->state=0;
						recon_cli(ptrcli);
						break;
					}
					if (ptrcli->callback != NULL) {
						ptrcli->callback(buffer, nread, (void*)ptrntr);
					}
				}
			}
			if (events[i].events & EPOLLOUT) {
				if (ptrcli->cli.state != 2) {
					if (!(ret = testcon_cli(fd))) {
						/*ENTER RECONNECT PROCESS*/
						epoll_ctl(stream->efd, EPOLL_CTL_DEL, fd, NULL);
						close(fd);
						recon_cli(ptrcli);
						break;
					}
					if (ret == 2){
						ptrcli->cli.state = 2;
						ptrntr->state=1;
						/*connect success!*/
						openntrip(ptrntr);
					}
					if (ret < 2)
						break;
				}
				send_packet(ptrcli);
			}
		}
	}
	return 1;
}

int main(){
	stream_t stream;
	startntripcli(&stream,ntrrecv_callback,1);
}










