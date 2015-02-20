/*
 * rtsp proxy part for alexte/sat-ip-proxy
 *
 */

#include<stdio.h>
#include<getopt.h>
#include<unistd.h>
#include<time.h>
#include<sys/poll.h>
#include<syslog.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<netinet/in.h>
#include<netdb.h>
#include<error.h>
#include<errno.h>
#include<string.h>

#define MAXOPENFDS 20000

char *prg;
int debug=0;
char *port="554";
char *srvip="0.0.0.0";
char *target;
int idletimeout=120;

void usage()
{
    fprintf(stderr,"usage: %s [-d] [-i <srvip>] [-p <port>] <target>\n"
	           "    srvip: ip to listen to (default 0.0.0.0 = any)\n"
		   "    port: tcp port to listen and connect to rtsp (default 554)\n",prg);
}

int prepare_socket(char *srvip,char *port)
{
    struct sockaddr_in sin={AF_INET};
    struct servent *pservent;
    struct hostent *phent;
    int portnr,x;
    int accept_s;

    portnr=atoi(port);
    if (portnr==0)
    {
        pservent=getservbyname(port,"tcp");
        if (pservent==NULL) 
        { fprintf(stderr,"get tcp port failed\n"); return -1;  }
        portnr=ntohs(pservent->s_port);
    }
    sin.sin_port= htons(portnr);
    phent=gethostbyname(srvip);
    if (phent==NULL) { fprintf(stderr,"gethostbyname(%s) failed\n",srvip); return -1;  }
    memmove(&sin.sin_addr,phent->h_addr,sizeof(sin.sin_addr));

    accept_s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (accept_s<0) { fprintf(stderr,"socket() failed\n"); return -1;  }

    x=1;
    setsockopt(accept_s, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));

    if (bind(accept_s, (struct sockaddr *)&sin, sizeof sin) < 0) 
    { fprintf(stderr,"bind() failed\n"); return -1;  }

    if (listen(accept_s, 500) < 0) 
    { fprintf(stderr,"listen() failed\n"); return -1;  }

    return accept_s;
}

struct pollfd lfd[MAXOPENFDS];
time_t lastact[MAXOPENFDS];
unsigned long srcip[MAXOPENFDS];
int nfd;

char *inet_ntoa(unsigned long ip)
{
    static char buf[16];

    sprintf(buf,"%lu.%lu.%lu.%lu", ip&0xff, ip>>8&0xff, ip>>16&0xff, ip>>24);
    return buf;
}

char *reasonstr[]={"OK","Timeout","Hangup"};

void inline dropconnection(int n,int reason)
{
    int i;
    time_t now;

    if (debug)
    {
    	time(&now);
        fprintf(stderr,"%ld disconnect from %s: %s (%d/%d)\n",now,inet_ntoa(srcip[n]),reasonstr[reason],n,nfd);
    }
    close(lfd[n].fd);
    for(i=n;i<nfd;i++) lfd[i]=lfd[i+1];
    nfd--;
}

void poll_loop(int accsock)
{
    int i,nret,newfd,len;
    time_t now,lastcollect;
    struct sockaddr_in sin;
    unsigned int sinlen;
    long long nc=0; // number of connections
    char buf[2049];

    time(&now);
    lfd[0].fd=accsock;
    lfd[0].events=POLLIN|POLLPRI;
    lfd[0].revents=0;
    lastact[0]=now;
    nfd=1;

    lastcollect=now;

    while (1)
    {
        nret=poll(lfd,nfd,2000);
        time(&now);
        switch(nret)
        {
            case 0:     // timeout
                if (debug>2) fprintf(stderr,"poll returned from timeout (%d open sockets)\n",nfd);
                break;
            case -1:    // error
                fprintf(stderr,"poll failed %s\n",strerror(errno));
                return;
            default:    // real socket events
                if (debug>2) fprintf(stderr,"nfd returned %d\n",nret);
                if (lfd[0].revents&(POLLERR|POLLHUP|POLLNVAL))          // an error occured ??
                { fprintf(stderr,"poll accept-fd failed %s\n",strerror(errno)); return; }
                if (lfd[0].revents&POLLIN && nfd==MAXOPENFDS) 
                { nret--; fprintf(stderr,"maximum session number reached (%d)\n",MAXOPENFDS); }
                if (lfd[0].revents&POLLIN && nfd<MAXOPENFDS)            // new connection coming in
                {
                    nret--;
                    newfd=accept(accsock,(struct sockaddr *)&sin, &sinlen);
                    if (newfd<0) fprintf(stderr,"accept socket failed %s\n",strerror(errno));
                    else
                    {
                        lfd[nfd].fd=newfd;
                        lfd[nfd].events=POLLIN|POLLPRI;
                        lfd[nfd].revents=0;
                        lastact[nfd]=now;
                        srcip[nfd]=sin.sin_addr.s_addr;
                        nfd++; nc++;
                    }
                }
                for(i=1;i<nfd && nret>0;i++,nret--)                     // data coming in ?
                {
                    if (lfd[i].revents&POLLHUP) dropconnection(i,2);
                    if (lfd[i].revents&POLLERR) dropconnection(i,2);
                    if (lfd[i].revents&POLLNVAL) dropconnection(i,2);
                    if (lfd[i].revents&POLLIN) 
                    {
                        len=read(lfd[i].fd,buf,2048);
                        if (len<0) dropconnection(i,2);
                        else
                        {
                            buf[len]=0;
                            write(lfd[i].fd,"OK Got It\n",strlen("OK Got It\n"));
                            dropconnection(i,0);
                        }
                    }
                }
                // if (debug) sleep(1);
        }
        if (lastcollect<now-5)
        {
           if (debug>2) fprintf(stderr,"dropping sessions older than %d seconds\n",idletimeout);
           for (i=1;i<nfd;) 
                if (lastact[i]+idletimeout<now) dropconnection(i,1); 
                else i++;
           lastcollect=now;
        }
    }
}

int main(int argc,char **argv)
{
    int ch,accsock; 

    prg=argv[0];
    while ((ch=getopt(argc,argv,"di:p:"))!= EOF)
    {
        switch(ch)
        {
            case 'd':   debug++; break;
            case 'i':   srvip=optarg; break;
            case 'p':   port=optarg; break;
            default:    usage(); exit(1); 
        }
    }
    argc-=optind;
    argv+=optind;

    if (argc!=1) { usage(); exit(1);  }
    
    target=argv[0];

    accsock=prepare_socket(srvip,port);
    if (accsock==-1) exit(3);

    poll_loop(accsock);

    exit(0);

}

