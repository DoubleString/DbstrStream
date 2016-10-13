/*
 * DbstrNtrip.c
 *
 *  Created on: 2016年10月7日
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
#include <string.h>
#include "list.h"
#include "DbstrNtrip.h"

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
} ntrip_cli_t;

typedef struct {
	struct list_head list;
	ntrip_cli_t ntrcli;
} ntrip_cli_list;




/*structure for ntrip caster & server*/
typedef struct{
	int type;  /*0:client 1:server*/
	int state; /*0:unauthorized 1:authorized*/
	char mountpoint[maxstrsize];
	void* host;
	tcpcli_t tcpcli;
	int bhandle;
}ntrip_svr_unit;


typedef struct{
	struct list_head list;
	void* host;
	void* con_list;  /*clients for the mountpoint corresponding to the server*/
	ntrip_svr_unit *unit;
}ntrip_svr_unit_list;

typedef struct{
	struct list_head list;
	char mountpoint[maxstrsize];
	ntrip_svr_unit_list cli_list;
}ntrip_svr_con_list;

typedef struct{
	ntrip_svr_con_list cliHead;


	ntrip_svr_unit_list svrHead;  /*list queue for server */
	ntrip_svr_unit_list halfHead; /*list queue for connected client*/

	void* host;
	tcp_t svr;
}ntrip_caster_t;

typedef struct{

}opt_t;


typedef struct {
	int type;
	int mode;
	epoll_t efd;
	union {
		tcpcli_list *cliHead;
		tcpsvr_t *svr;
		ntrip_cli_list *ntrHead;
		ntrip_caster_t *ntrcaster;
	} prot;
	lock_t synlock;
} stream_t;
#ifdef WIN32

#else
static int errsock(void) {
	return errno;
}
#endif


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
void timinc(int jd, double sec, double delt, int* jd1, double* sec1) {
	*sec1 = sec + delt;
	int inc = (int) (*sec1 / 86400.0);
	*jd1 = jd + inc;
	*sec1 = *sec1 - inc * 86400.0;
	if (*sec1 >= 0)
		return;
	*jd1 = *jd1 - 1;
	*sec1 = *sec1 + 86400;
}
int run_tim() {
	struct tm *ptr;
	time_t rawtime;
	time(&rawtime);
	ptr=gmtime(&rawtime);

	int iyear,idoy;


	int mjdgps,mjdtai;
	double sodgps,sodtai;

	int mjd=modified_julday(ptr->tm_year+1900,ptr->tm_mon+1,ptr->tm_mday);

	double sod=3600*ptr->tm_hour+60*ptr->tm_min+ptr->tm_sec;


	timinc(mjd,sod,36,&mjdtai,&sodtai);

	timinc(mjdtai,sodtai,19*-1,&mjdgps,&sodgps);

	mjd2doy(mjdgps,&iyear,&idoy);

	return idoy;
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
static int send_packet(tcpcli_t *cli) {
	lock(&(cli->synccli));
	packet_list* pos = NULL;
	int nsend, nrem, sumsd;
	char buff[payloadsize];

	while (!list_empty(&cli->sndpkt.list)) {
		printf("enter send packet!\n");
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

	return cli->snredy;
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
	return cli->snredy;
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

static void openntrip(ntrip_cli_t* ptrntr){
	char cmd[maxstrsize],*base64,req[maxstrsize];
	sprintf(cmd,"%s:%s",ptrntr->usr,ptrntr->psd);
	base64=base64_encode(cmd,strlen(cmd));
	sprintf(req,"GET /%s HTTP/1.0\r\nUser-Agent: NTRIP GNSSInternetRadio/1.2.0\r\nAuthorization: Basic %s\r\n\r\n",
			ptrntr->mountpoint,base64);
	send_nb(ptrntr->cli,req,strlen(req));
}
void recon_callback(void* arg) {
	tcpcli_t* ptrcli = (tcpcli_t*) arg;

	ntrip_cli_t* ntrcli = (ntrip_cli_t*) ptrcli->host;
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

	ev.data.ptr = (void*) ptrcli;

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
			((ntrip_cli_t*)cli->host)->state=0;
			/*create thread failed remove the client*/
//			discont_cli((stream_t*)((ntrip_t*) cli->host)->host, cli);
			return 0;
		}
	}
}
static void ntrrecv_callback(char* buffer,int n,void* arg){
	ntrip_cli_t* ptrntr = (ntrip_cli_t*) arg;
#define maxcli 100

	static int fd[maxcli]={0};
	static FILE* fp[maxcli]={NULL};
	static int id[maxcli],i;
	if(ptrntr->state<=0)
		return;
	if(ptrntr->state==1){
		if (strstr(buffer, ICY_OK) != NULL){
			printf("ICY 200 OK!\n");
			ptrntr->state=2;
		}
		else if (strstr(buffer, ICY_UN) != NULL) {
			printf("登录信息错误,请检查用户名,密码,挂载点是否输入正确!\n");

		} else if (strstr(buffer, ICY_SOURCE) != NULL) {
			printf("请选择挂载点!");
		}
		return;
	}
	for(i=0;i<maxcli;i++){
		if(fd[i]==0||fd[i]==ptrntr->cli->cli.sock){
			break;
		}
	}
	if(fd[i]==0){
		fd[i]=ptrntr->cli->cli.sock;
	}

	int iday=run_tim();
	char buff[1024];
	if (id[i] != iday) {
		if(fp[i])
			fclose(fp[i]);
		sprintf(buff,"savfile_%s_%d",ptrntr->mountpoint,ptrntr->cli->cli.sock);
		if(!(fp[i]=fopen(buff,"w"))){
			printf("cant open file to write :%s \n",buff);
			exit(1);
		}
		id[i]=iday;
	}

	fwrite(buffer,sizeof(char),n,fp[i]);
	fflush(fp[i]);
}

/*********************************************************NTRIP CLIENT*************************************************************************************/

static int startntripcli(stream_t *stream, dataRecvCallback callback, int ncli) {
	tcpcli_t *cli,*ptrcli;
	ntrip_cli_t *ptrntr,ptrntr0={{0}};
	int nfd, i, fd, nread, ret;
	char buffer[payloadsize];
	struct epoll_event events[MAXEPOLL], ev;
	/********************************INIT-CONFIG***********************************************/
	stream->mode = MODE_NTRIPCLI;
	stream->type = STREAM_TCP;
	stream->efd = epoll_create(MAXEPOLL);

	stream->prot.ntrHead=(ntrip_cli_list*)malloc(sizeof(ntrip_cli_list));
	list_init(&stream->prot.ntrHead->list);
	initlock(&stream->synlock);
	/*********************************READ-CONFIG-FILE*****************************************/

	/******************************************************************************************/
	lock(&stream->synlock); /*lock because reconnect thread may change the stream.prot.cliHead*/
	for (i = 0; i < ncli; i++) {
		if (!(cli = opentcpcli("59.172.4.52",7005))) {
			printf("failed to opencli!\n");
			continue;
		}

		printf("opencli successfully! fd id:%d \n", cli->cli.sock);
		if (callback != NULL)
			cli->callback = callback;

		/*add to the stream*/
		ntrip_cli_list* ntrlist = (ntrip_cli_list*)malloc(sizeof(ntrip_cli_list));
		ntrlist->ntrcli=ptrntr0;

		/*********************************************add ntrip configure for test********************************************************/
//		switch(i){
//		case 0:
//			strcpy(ntrlist->ntrcli.mountpoint,"TJ01");
//			strcpy(ntrlist->ntrcli.usr,"dd");
//			strcpy(ntrlist->ntrcli.psd,"111111");
//			strcpy(cli->cli.saddr,"127.0.0.1");
//			cli->cli.port=2101;
//		break;
//		case 1:
//			strcpy(ntrlist->ntrcli.mountpoint,"QLZ2");
//			strcpy(ntrlist->ntrcli.usr,"dd");
//			strcpy(ntrlist->ntrcli.psd,"111111");
//			strcpy(cli->cli.saddr,"59.175.223.165");
//			cli->cli.port=2101;
//
//			break;
//		case 2:
//			strcpy(ntrlist->ntrcli.mountpoint,"ION1");
//			strcpy(ntrlist->ntrcli.usr,"dd");
//			strcpy(ntrlist->ntrcli.psd,"111111");
//			strcpy(cli->cli.saddr,"59.175.223.165");
//			cli->cli.port=2101;
//			break;
//		}

		strcpy(ntrlist->ntrcli.mountpoint,"TJ03");
		strcpy(ntrlist->ntrcli.usr,"dd");
		strcpy(ntrlist->ntrcli.psd,"111111");
		strcpy(cli->cli.saddr,"59.172.4.52");
		cli->cli.port=7005;
		/******************************************************************************************************************************/
		ntrlist->ntrcli.cli=cli;
		ntrlist->ntrcli.host=(void*)stream;
		list_add_tail(&ntrlist->list,&stream->prot.ntrHead->list);
		cli->host = (void*)&ntrlist->ntrcli;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		ev.data.ptr = (void*)cli;
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
			ptrcli = (tcpcli_t*)events[i].data.ptr;

			ptrntr=(ntrip_cli_t*)ptrcli->host;

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







/**************************************NTRIP-CASTER******************************************************/
static int accept_nb(ntrip_caster_t* ptrcas) {
	struct sockaddr_in addr;
	struct epoll_event ev;
	socklen_t len = sizeof(struct sockaddr);
	stream_t *stream=(stream_t*)ptrcas->host;
	/*if it is ET mode ,here should be a loop*/
	while (1) {
		int fd = accept(ptrcas->svr.sock, (struct sockaddr*) &addr, &len);
		if (fd == -1 && errno == EWOULDBLOCK)
			break;
		if (!setsock(fd)){
			/*can't set no-block */
			printf("cant set no-block socket!\n");
			close(fd);
			continue;
		}
		/***************add client information**************************/
		ntrip_svr_unit *ptrunit=(ntrip_svr_unit*)malloc(sizeof(ntrip_svr_unit));

		tcpcli_t* cli=&ptrunit->tcpcli;
		ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
		ev.data.ptr = (void*)ptrunit;
		if (epoll_ctl(stream->efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
			perror("epoll_ctl failed! server break down!\n");
			exit(1);
		}
		/******************fill the tcpcli information******************/
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
		/***************************************************************/

		ptrunit->state=0;

		ntrip_svr_unit_list* ptrunitlist = (ntrip_svr_unit_list*)malloc(sizeof(ntrip_svr_unit_list));
		ptrunitlist->unit=ptrunit;

		/*update the unit_list host*/
		ptrunitlist->host=(void*)&ptrcas->halfHead;
		ptrunit->host=(void*)ptrunitlist;

		/*update the unit bhandle*/
		ptrunit->bhandle=0;
		/*********************add to the ntrip_cast_t queue******************/
		list_add_tail(&ptrunitlist->list,&(ptrcas->halfHead.list));

		printf("accept client :%d \n",ptrunit->tcpcli.cli.sock);
	}
	return 1;
}
static ntrip_caster_t* openntripsvr(opt_t* opt){

	ntrip_caster_t *ntrsvr,ntrsvr0={{0}};
	if(!(ntrsvr = (ntrip_caster_t*)malloc(sizeof(ntrip_caster_t)))){
		perror("can't malloc memory for ntrip caster!\n");
		exit(1);
	}

	*ntrsvr=ntrsvr0;
	strcpy(ntrsvr->svr.saddr,"59.172.4.52");
	ntrsvr->svr.port=7005;
	if(!gentcp(&ntrsvr->svr,0)){
		free(ntrsvr);
		ntrsvr=NULL;
		return NULL;
	}
	/*initial connected client list*/
	list_init(&ntrsvr->cliHead.list);
	list_init(&ntrsvr->svrHead.list);
	list_init(&ntrsvr->halfHead.list);
	return ntrsvr;

}
static ntrip_svr_con_list* get_conlist(ntrip_caster_t* ptrcas,char* mount){
	ntrip_svr_con_list* conHead,*ptrcon;
	conHead = &ptrcas->cliHead;
	list_for_each_entry(ptrcon,&conHead->list,list){
		if(!strncmp(ptrcon->mountpoint,mount,strlen(mount))){
			return ptrcon;
		}
	}
	return NULL;
}

static void gen_sourcetable(ntrip_caster_t* ptrcas,char* buffer){
	ntrip_svr_unit_list* ptrsvr,*ptrsvrpos;
	char mout[1024],msg[1024];
	ptrsvr = &ptrcas->svrHead;
	strcpy(msg,"SOURCETABLE 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %d\r\n");
	int len=0;
	list_for_each_entry(ptrsvrpos,&ptrsvr->list,list){
		sprintf(mout,"STR;%s;;RTCM3; ; ; ; ; ; ; ; ; ; ; ; ; ; ; \r\n",((ntrip_svr_con_list*)ptrsvrpos->con_list)->mountpoint);
		len+=strlen(mout);
		strcat(msg,mout);
	}
	strcat(msg,"ENDSOURCETABLE\r\n");
	len+=strlen("ENDSOURCETABLE\r\n");
	sprintf(buffer,msg,len);
}
static int parse_request(ntrip_svr_unit* ptrunit,ntrip_caster_t* ptrcas,char* buffer,int n,char* replymsg){
	int ret=0;
	char mountpoint[maxstrsize]={0};
	char authpsd[maxstrsize]={0};
	char msg[maxstrsize*2]={0};
	ntrip_svr_unit_list* ptrunitlist = (ntrip_svr_unit_list*)ptrunit->host;
	ntrip_svr_con_list* conlist=NULL;
	/*update the handle tag*/
	ptrunit->bhandle=1;

	char* ptrcliReq=NULL,*ptrReq=NULL;
	char* ptrsvrReq=NULL;

	/*Compatible to other forms of request*/
	strncpy(msg,buffer,n);
	ptrReq = lower_string(msg);
	ptrcliReq=strstr(ptrReq,"get");
	ptrsvrReq=strstr(ptrReq,"source");
	int index=-1;

	if(ptrcliReq){
		index=index_string(ptrReq,*ptrcliReq);
		if(index!=0){
			strcpy(replymsg,"401 Unauthorized");
			return 0;
		}
		while(buffer[index]!=' '){
			if (buffer[index] >= 'A' && buffer[index] <= 'Z')
				buffer[index] += 32;
			index++;
		}
	}else if(ptrsvrReq){
		index=index_string(ptrReq,*ptrsvrReq);
		if(index!=0){
			strcpy(replymsg,"401 Unauthorized");
			return 0;
		}
		while(buffer[index]!=' '){
			if (buffer[index] >= 'A' && buffer[index] <= 'Z')
				buffer[index] += 32;
			index++;
		}
	}


	if(ptrcliReq){
		/*client part*/
		sscanf(strstr(buffer,"get"),"%*s%s",mountpoint);


		/*remove the /*/
		if(mountpoint[0]=='/'){
			char* ptr =mountpoint;
			while(*ptr!='\0'){
				*ptr=*(ptr+1);
				ptr++;
			}
		}
		printf("ntrip client request mountpoint:%s\n",mountpoint);
		if(!strstr(buffer,"Authorization: Basic")){
			/*generate source table message*/
			gen_sourcetable(ptrcas,replymsg);

			return 0;
		}else{
			sscanf(strstr(buffer,"Authorization: Basic"),"%*s%s",authpsd);

			if(0){
				/*failed to authorized*/
				sprintf(replymsg,"Server: NtripCaster/1.0\r\n"
					 "WWW-Authenticate: Basic realm=\"/%s\"\r\n"
					 "Content-Type: text/html\r\n"
					 "Connection: close\r\n"
					 "<html><head><title>401 Unauthorized</title></head><body bgcolor=black text=white\r\n"
								 "link=blue alink=red>\r\n"
					 "<h1><center>The server does not recognize your privileges to the requested entity\r\n"
								 "stream</center></h1>\r\n"
					 "</body></html>",mountpoint);
				return 0;
			}
		}
		conlist = get_conlist(ptrcas,mountpoint);
		if(conlist==NULL){
			gen_sourcetable(ptrcas,replymsg);
			return 0;
		}
		else{
			/*update the client*/
			list_del(&ptrunitlist->list);
			list_add_tail(&ptrunitlist->list,&conlist->cli_list.list);
			//update the configure
			ptrunit->state=1;
			ptrunit->type =0;
			/*change the host address*/
			ptrunitlist->host=(void*)&ptrcas->cliHead;

			/*send to the client*/
			sprintf(replymsg,"ICY 200 OK");
			send_nb(&ptrunit->tcpcli,replymsg,strlen(replymsg));

			return 1;
		}
	}else if(ptrsvrReq){
		char psd[256];
		sscanf(strstr(buffer,"source"),"%*s%s%s",psd,mountpoint);
		/*remove the /*/
		if(mountpoint[0]=='/'){
			char* ptr =mountpoint;
			while(*ptr!='\0'){
				*ptr=*(ptr+1);
				ptr++;
			}
		}
		if(strlen(mountpoint)>100){
			strcpy(replymsg,"401 Unauthorized : mountpoint size too long!\n");
			return 0;
		}
		//check for mountpoint int the caster


		printf("svr connected success! psd: %s mountpoint: %s \n",psd,mountpoint);

		/*server part*/
		//delete from the original queue and add to the server queue
		list_del(&ptrunitlist->list);
		list_add_tail(&ptrunitlist->list,&ptrcas->svrHead.list);

		//create the corresponding connected-list and init the variable
		conlist = (ntrip_svr_con_list*)malloc(sizeof(ntrip_svr_con_list));
		list_init(&conlist->cli_list.list);
		strcpy(conlist->mountpoint,mountpoint);
		list_add_tail(&conlist->list,&ptrcas->cliHead.list);

		//update the list host
		ptrunitlist->con_list=(void*)conlist;
		ptrunitlist->host=(void*)&ptrcas->svrHead;

		//update the configure
		ptrunit->state=1;
		ptrunit->type=1;

		//remember to send the corresponding answer message to the server
		sprintf(replymsg,"ICY 200 OK");
		send_nb(&ptrunit->tcpcli,replymsg,strlen(replymsg));
		return 1;
	}
	return 1;

}
/*no source server input*/
static void svr_close_conlist(ntrip_svr_con_list* conlist,ntrip_caster_t* ptrcas){
	ntrip_svr_unit_list* ptrunitHead,*ptrunitpos;
	ptrunitHead = &conlist->cli_list;

	stream_t* stream = (stream_t*)ptrcas->host;

	/*client packet*/

	packet_list* ptrpakHead,*ptrpakpos;

	while (!list_empty(&ptrunitHead->list)) {
		ptrunitpos = list_entry(ptrunitHead->list.next, typeof(*ptrunitpos), list);

		//REMOVE FROM THE SERVER
		epoll_ctl(stream->efd,EPOLL_CTL_DEL,ptrunitpos->unit->tcpcli.cli.sock,NULL);
		close(ptrunitpos->unit->tcpcli.cli.sock);

		/************free the unsent message***********************/
		ptrpakHead=&ptrunitpos->unit->tcpcli.sndpkt;

		while(!list_empty(&ptrpakHead->list)){
			ptrpakpos = list_entry(ptrpakHead->list.next,typeof(*ptrpakpos),list);
			list_del(&ptrpakpos->list);
			free(ptrpakpos);
		}
		/**********************************************************/

		list_del(&ptrunitpos->list);
		free(ptrunitpos->unit);
		free(ptrunitpos);
	}

	list_del(&conlist->list,&ptrcas->cliHead.list);
	free(conlist);
	conlist=NULL;
}

/*send to the client*/
static void send_cli(ntrip_svr_con_list* conlist,char* buffer,int n){
	ntrip_svr_unit_list *ptrunitHead,*ptrunitpos;
	ptrunitHead=&conlist->cli_list;

//	while(!list_empty(&ptrunitHead->list)){
//		ptrunitpos=list_entry(ptrunitHead->list.next,typeof(*ptrunitpos),list);
//		send_nb(&ptrunitpos->unit->tcpcli,buffer,n);
//	}
	list_for_each_entry(ptrunitpos,&ptrunitHead->list,list){
		send_nb(&ptrunitpos->unit->tcpcli,buffer,n);
	}
}
static int handle_request(ntrip_svr_unit* ptrunit,ntrip_caster_t* ptrcas){
	char buffer[maxstrsize*2];
	char replymsg[maxstrsize];
	int nread;
	while(1){
		nread = recv(ptrunit->tcpcli.cli.sock, buffer, sizeof(buffer), 0);
		if (nread < 0
				&& ( errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)) {
			return 1;
		} else if (nread <= 0) {
			stream_t* stream = (stream_t*)ptrcas->host;
			epoll_ctl(stream->efd, EPOLL_CTL_DEL, ptrunit->tcpcli.cli.sock, NULL);
			close(ptrunit->tcpcli.cli.sock);
			printf("unauthorized client close stream ! fd:%d\n",ptrunit->tcpcli.cli.sock);
			ntrip_svr_unit_list* ptrunitlist = (ntrip_svr_unit_list*)ptrunit->host;
			list_del(&ptrunitlist->list);
			free(ptrunit);
			free(ptrunitlist);
			ptrunit=NULL;
			return 0;
		}
		if(nread<sizeof(buffer))
			buffer[nread]='\0';

		printf("the request msg is:\n");
		printf("%s\n",buffer);

		if(!(parse_request(ptrunit,ptrcas,buffer,nread,replymsg))){
			/*	 failed to authorized	*/

			printf("authorized failed!\n");
			/*generate sending-back message */

			if(send_nb(&ptrunit->tcpcli,replymsg,strlen(replymsg))){
				/*remove from the half-connected list*/
				ntrip_svr_unit_list* ptrunitlist =
						(ntrip_svr_unit_list*) ptrunit->host;
				list_del(&ptrunitlist->list);

				/*close the client*/
				stream_t* stream = (stream_t*)ptrcas->host;
				epoll_ctl(stream->efd, EPOLL_CTL_DEL, ptrunit->tcpcli.cli.sock, NULL);
				close(ptrunit->tcpcli.cli.sock);

				free(ptrunit);
				free(ptrunitlist);
				ptrunit=NULL;
				return 0;
			}
		}
	}
}
static int handle_source(ntrip_svr_unit* ptrunit,ntrip_caster_t* ptrcas){
	char buffer[maxstrsize];
	int nread;
	ntrip_svr_unit_list* ptrunitlist = (ntrip_svr_unit_list*)ptrunit->host;
	ntrip_svr_con_list* ptrconlist;
	stream_t* stream=(stream_t*)ptrcas->host;
	while(1){
		nread = recv(ptrunit->tcpcli.cli.sock, buffer, sizeof(buffer), 0);
		if (nread < 0
				&& ( errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)) {
			return 1;
		} else if (nread <= 0) {
			switch(ptrunit->type){
			/*client closed*/
			case 0:
				/*update the epoll configure*/
				printf("authorized client close stream ! fd:%d\n",ptrunit->tcpcli.cli.sock);

				epoll_ctl(stream->efd, EPOLL_CTL_DEL, ptrunit->tcpcli.cli.sock, NULL);
				close(ptrunit->tcpcli.cli.sock);

				/*free the unit*/
				list_del(&ptrunitlist->list);
				free(ptrunit);
				free(ptrunitlist);
				ptrunit=NULL;
				return 0;
			/*server closed*/
			case 1:
				/*free the connected list*/
				printf("authorized server close stream ! fd:%d\n",ptrunit->tcpcli.cli.sock);

				ptrconlist = (ntrip_svr_con_list*)ptrunitlist->con_list;
				svr_close_conlist(ptrconlist,ptrcas);

				/*free the server*/
				epoll_ctl(stream->efd, EPOLL_CTL_DEL, ptrunit->tcpcli.cli.sock, NULL);
				close(ptrunit->tcpcli.cli.sock);
				/*free the unit*/
				list_del(&ptrunitlist->list);
				free(ptrunit);
				free(ptrunitlist);
				ptrunit=NULL;
				return 0;
			}
			break;
		}
		/*ignore the authorized client sending-message*/
		if(ptrunit->type==0)
			continue;

		ptrconlist=(ntrip_svr_con_list*)ptrunitlist->con_list;
		send_cli(ptrconlist,buffer,nread);
	}
}
static void startntripcaster(stream_t* stream){

	ntrip_caster_t* ntrsvr;

	int nfd, i,bfree;

	struct epoll_event events[MAXEPOLL], ev;

	/****************Initial Epoll Configure****************************/
	stream->mode=MODE_NTRIPSVR;
	stream->type=STREAM_TCP;
	stream->efd = epoll_create(MAXEPOLL);

	/******************************************************************/
	if(!(ntrsvr=openntripsvr(NULL))){
		return;
	}
	printf("opensvr successfully!\n");

	ntrsvr->host=(void*)stream;

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd=ntrsvr->svr.sock;
	epoll_ctl(stream->efd, EPOLL_CTL_ADD, ntrsvr->svr.sock, &ev);
	while(1){
		nfd = epoll_wait(stream->efd, events, MAXEPOLL, -1);
		for(i=0;i<nfd;i++){
			if(events[i].data.fd==ntrsvr->svr.sock&&(events[i].events&EPOLLIN)){
					accept_nb(ntrsvr);
			}else if(events[i].events&EPOLLIN){
				ntrip_svr_unit* ptrunit = (ntrip_svr_unit*)events[i].data.ptr;
				switch(ptrunit->state){
				/*client or server request*/
				case 0:
					bfree=handle_request(ptrunit,ntrsvr);
					break;
				/*server source stream*/
				case 1:
					bfree=handle_source(ptrunit,ntrsvr);
					break;
				}
				if(!bfree)
					continue;
			}if(events[i].events&EPOLLOUT){
				ntrip_svr_unit* ptrunit = (ntrip_svr_unit*)events[i].data.ptr;

				if(ptrunit->bhandle&&send_packet(&ptrunit->tcpcli)){
					if(ptrunit->state==0){
						epoll_ctl(stream->efd,EPOLL_CTL_DEL,ptrunit->tcpcli.cli.sock,NULL);
						close(ptrunit->tcpcli.cli.sock);
						ntrip_svr_unit_list* ptrunitlist = (ntrip_svr_unit_list*)ptrunit->host;
						list_del(&ptrunitlist->list);

						free(ptrunit);
						free(ptrunitlist);
						ptrunit=NULL;
					}
				}
			}
		}
		/*clear the timeout clients*/
	}
}
/*****************************************NTRIP-SERVER******************************************************************/
static void startntripsvr(stream_t* stream){





}
int main(int argc,char* args[]){


	if(strstr(args[1],"caster")){
		stream_t stream;
		startntripcaster(&stream);
	}else if(strstr(args[1],"cli")){
		stream_t stream;
		startntripcli(&stream,ntrrecv_callback,1);
	}else if(strstr(args[1],"svr")){
		stream_t stream;
		ntripsvr(&stream,NULL,1);
	}
}



