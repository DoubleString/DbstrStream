/*
 * stream_mulpth.c
 *
 *  Created on: 2016年10月3日
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
#include <unistd.h>
#include <linux/tcp.h>
#include "list.h"
#define socket_t int
#define epoll_t int
#define closesocket close

#define STREAM_TCP 1

#define MODE_SVR (STREAM_TCP+1)
#define MODE_CLI (MODE_SVR+1)

#define payloadsize 1024

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
static int errsock(void){return errno;}
#define MAXEPOLL 1024
typedef void (*dataRecvCallback)(char *buff, int n,int fd);
lock_t pth;
typedef struct {
	int state; /* 0:close 1:wait 2:connect*/
	char saddr[256];
	int port;
	struct sockaddr_in addr;
	socket_t sock;
	int tcon; /*reconnect time (ms) (-1:never,0:now)*/
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

	pthread_t pthid;
	dataRecvCallback callback; /*callback for client mode*/
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
	int type;
	int mode;

	epoll_t efd;
	void* prot;
} stream_t;
static int gentcp(tcp_t *tcp, int type) {
	struct hostent *hp;
	/* generate socket */
	if ((tcp->sock = socket(AF_INET, SOCK_STREAM, 0)) == (socket_t) -1) {
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
			printf("bind error (%d) : %d\n", errsock(), tcp->port);
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
			tcp->tcon = ticonnect;
			tcp->tdis = tickget();
			return 0;
		}
		memcpy(&(tcp->addr.sin_addr), hp->h_addr, hp->h_length);
	}

	tcp->state = 1;
	tcp->tact = tickget();

	return 1;
}
static void freetcpcli(tcpcli_t *cli){

	/*free packet memory
	 * there is no need to lock the synccli anymore because the syncsvr is locked
	 * */

//	lock(&(cli->synccli));
	packet_list *pos=NULL;
	while(!list_empty(&cli->sndpkt.list)){
		pos = list_entry(cli->sndpkt.list.next,typeof(*pos),list);
		list_del(&pos->list);
		free(pos);
		pos=NULL;
	}
	/*free tcpcli_t memory*/
//	unlock(&(cli->synccli));

	free(cli);
	cli=NULL;
}

static tcpsvr_t* opentcpsvr(char *addr, int port) {
	tcpsvr_t *tcpsvr, tcpsvr0 = { { 0 } };
	if ((tcpsvr = (tcpsvr_t*) malloc(sizeof(tcpsvr_t))) == NULL) {
		printf("malloc error\n");
		return NULL;
	}
	*tcpsvr = tcpsvr0;

	tcpsvr->svr.port=port;
	inet_aton(addr,&tcpsvr->svr.addr.sin_addr);
	if (!gentcp(&tcpsvr->svr, 0)) {
		free(tcpsvr);
		tcpsvr = NULL;
		return NULL;
	}
	tcpsvr->svr.tcon = 0;
	initlock(&tcpsvr->syncsvr);
	list_init(&(tcpsvr->clients.list));
	return tcpsvr;
}
static void callback(char* buffer, int n, int fd) {

	lock(&pth);
#define maxfd 50
	static FILE* fp[maxfd];
	static int fdarray[maxfd] = { 0 };
	static int totalsize[maxfd]={0};
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

	totalsize[i]+=n;
	int len=fwrite(buffer, sizeof(char), n, fp[i]);
	if(len<n){
		printf("writing less than n for client %d\n",fd);
	}
	fflush(fp[i]);


	//printf("sock:%d totalsize :%d \n",fd,totalsize[i]);
	unlock(&pth);

}
static void pth_recv(void* arg) {
	tcpcli_t *cli = (tcpcli_t*) arg;
	printf("thread for client %d create success!\n",cli->cli.sock);
	int len;
	char buffer[payloadsize];
	while (1) {
		len = recv(cli->cli.sock, buffer, payloadsize, 0);
		if (len <= 0) {
			printf("client %d close the stream !\n",cli->cli.sock);
			cli->cli.state = 0;
			close(cli->cli.sock);
			break;
		}

		if (cli->callback != NULL) {
			cli->callback(buffer, len,cli->cli.sock);
		}
	}
}

static void accept_svr(tcpsvr_t *svr) {
	struct sockaddr_in addr;
	socklen_t len = sizeof(struct sockaddr);

	int fd = accept(svr->svr.sock, (struct sockaddr*) &addr, &len);

	if (fd != -1) {
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
		cli->cli.tcon = cli->reconinv ? -1 : 0;

		cli->cli.tact = tickget();

		cli->callback=callback;

		memcpy(&(cli->cli.addr), &addr, sizeof(addr));
		strcpy(cli->cli.saddr, inet_ntoa(addr.sin_addr));

		int mode=1;

	    if (setsockopt(cli->cli.sock,IPPROTO_TCP,TCP_NODELAY,(const char *)&mode,sizeof(mode))==-1) {
	        printf(1,"setsock: setsockopt error 3 sock=%d err=%d\n",cli->cli.sock,errsock());

	    }
		tcpcli_list* tcpcli = (tcpcli_list*) malloc(sizeof(tcpcli_list));
		tcpcli->client = cli;
		list_add_tail(&tcpcli->list, &(svr->clients.list));

		printf("new client accept!\n");
		if (-1 == pthread_create(&cli->pthid, NULL, pth_recv, (void*) cli)) {
			printf("cant create thread for socket %d now exit!\n", fd);
			exit(1);
		}

	}
}
static int start_up(void *arg) {
	stream_t *stream = (stream_t*) arg;
	tcpsvr_t* svr;
	char buffer[payloadsize];
	int nfd, i, curfd, nread;
	if (!(svr = opentcpsvr("127.0.0.1",8002))) {
		printf("cant open tcpsvr!\n");
		return 0;
	}
	stream->prot=(void*)svr;
	while (1) {
		printf("accept process begin to work!\n");
		accept_svr(svr);
	}
}
int loadFile(FILE** fp){
	char buffer[payloadsize];
	printf("Please input the FILENAME:\n");
	scanf("%s", buffer);
	if (!(*fp = fopen(buffer, "r"))) {
		printf("cant open file %s to send!\n", buffer);
		return 0;
	}
	printf("openfile successfully!Please input enter to continue!\n");
	getchar();
	return 1;
}


int m_ain(int argc, char *args[]) {
	initlock(&pth);
	stream_t stream={0};
	pthread_t pth_main;
	tcpcli_list *cliHead;
	tcpcli_list *recu,*tmp;
	FILE *fp;
	char buffer[payloadsize];
	int len,sumsd,nsend,nrem,ncli;
	if (-1 == pthread_create(&pth_main, NULL, start_up, &stream)) {
		printf("cant create thread for main thread ! now exit!\n");
		exit(1);
	}
	printf("thread for main created success!\n");

	while(stream.prot==NULL){
		printf("enter to wait stream svr!\n");
		usleep(1000000);
	}
	printf("continue to send buffer!\n");
	tcpsvr_t *svr = (tcpsvr_t*)stream.prot;

	while (1) {
		if(!loadFile(&fp))
			continue;
		ncli;
		while(!feof(fp)){
			len=fread(buffer,sizeof(char),payloadsize,fp);
			cliHead= &svr->clients;
			recu = list_entry(cliHead->list.next, typeof(*recu), list);
			while (&recu->list != &cliHead->list) {
				/*update closed socket*/
				tmp = recu->client->cli.state == 0 ? recu : NULL;

				/*update timeout socket*/
				if (tmp == NULL) {

				}
				if(tmp==NULL){
					nrem=len;
					sumsd=0;
					while(nrem>0){
						/*send the whole packet*/
						nsend=send(recu->client->cli.sock,buffer+sumsd,nrem,0);
						sumsd+=nsend;
						nrem -=sumsd;
					}

				}
				recu = list_entry(recu->list.next, typeof(*recu), list);
				if (tmp) {
					printf("clear memory for client %d\n",tmp->client->cli.sock);
					/*free memory*/
					list_del(&tmp->list);
					freetcpcli(tmp->client);
					tmp = NULL;
				}
			}
		}

		fclose(fp);
	}
}

