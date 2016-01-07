/*
 * rtsp proxy part for alexte/sat-ip-proxy
 *
 */

#include<stdio.h>
#include<string.h>
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
#include<regex.h>
#include<arpa/inet.h>

#define MAXOPENFDS 2000
#define MAXREQUESTLEN 2000
#define MAXSESSIONS 300
#define MAXPIDS 100

#define TRUE 1
#define FALSE 0

char *prg;
int debug=0;
char *port="554";
char *srvip="0.0.0.0";
char *target;
int idletimeout=120;
int nr_sessions=0;
time_t now;

struct SESSION {
    char *id;
    char *cseq;
    struct in_addr client_ip;
    int pid[MAXPIDS];
    int npids;
    int client_port;
    int recv_port;
    int lastuse;
    int udp_fd;
    int udp2_fd;
} session[MAXSESSIONS];

void usage()
{
    fprintf(stderr,"usage: %s [-d] [-i <srvip>] [-p <port>] <target>\n"
	           "    srvip: ip to listen to (default 0.0.0.0 = any)\n"
		   "    port: tcp port to listen and connect to rtsp (default 554)\n",prg);
}

struct pollfd lfd[MAXOPENFDS];

struct LFD_M {
    char inbuf[MAXREQUESTLEN+1];
    int inbuf_offset;
    time_t lastact;
    enum { f_accept,f_client,f_server,f_udprcv} type;
    struct SESSION *sessionpointer;
    struct in_addr client_ip;
    struct sockaddr_in saddr;
    int deleted;
    char srvip[16];
} lfd_m[MAXOPENFDS];

int nfd;

void remove_lfd(int n)  	// mark as deleted first, cleanup later, to keep fd array 
{
    if (debug>1) fprintf(stderr,"remove_lfd(%d)\n",n);
    lfd_m[n].deleted=TRUE;
}

void dump_sessions();

void cleanup_lfd()
{
    int i,move=0;
    
    for (i=0;i<nfd;)
    {
	if (move>0) 
	{
	    lfd[i]=lfd[i+move];
	    lfd_m[i]=lfd_m[i+move];
	}
        if (lfd_m[i].deleted) move++;
	else i++;
    }
    nfd-=move;
}

void remove_lfd_by_fd(int fd)
{
    int i;
    for (i=0;i<nfd;i++)
	if(lfd[i].fd==fd) remove_lfd(i);
}

void bp(char *s)
{ puts(s); fflush(stdout); }

struct SESSION *start_session(struct in_addr client_ip,char *cseq)
{
    int n=nr_sessions;

    if (n>=MAXSESSIONS) return NULL;
    nr_sessions++;
    session[n].client_ip=client_ip;
    session[n].id=NULL;
    session[n].cseq=strdup(cseq);
    if (!session[n].cseq) return NULL;
    session[n].client_port=-1;
    session[n].recv_port=-1;
    session[n].lastuse=now;
    session[n].udp_fd=-1;
    session[n].udp2_fd=-1;
    session[n].npids=0;
    return &session[n];
}

struct SESSION *get_session(char *id)
{
    int i;
    if (!id) return NULL;
    for(i=0;i<nr_sessions;i++)
	if(session[i].id && !strcmp(session[i].id,id))
	{
	    session[i].lastuse=now;
	    return &session[i];
	}
    return NULL;
}

struct SESSION *get_session_by_cseq(char *cseq)
{
    int i;
    if (!cseq) return NULL;
    for(i=0;i<nr_sessions;i++)
	if(session[i].cseq && !strcmp(session[i].cseq,cseq)) return &session[i];
    return NULL;
}

void remove_session(char *id)
{
    int i;
    char *p;

    for(p=id;*p && *p==' ';p++);
    for (i=0;i<nr_sessions;i++)
	if (!strcmp(session[i].id,p)) break;
    if (i>=nr_sessions) return;

    if (session[i].udp_fd>=0) { close(session[i].udp_fd); remove_lfd_by_fd(session[i].udp_fd); }
    if (session[i].udp2_fd>=0) { close(session[i].udp2_fd); remove_lfd_by_fd(session[i].udp2_fd); }

    free(session[i].id);
    free(session[i].cseq);

    nr_sessions--;
    for (;i<nr_sessions-1;i++)
	session[i]=session[i+1];
}

void dump_sessions()
{
    int i,j;
    struct SESSION *s;

    if (nr_sessions>0) puts("\nSessions:");
    for (i=0;i<nr_sessions;i++)
    {
	s=&session[i];
	fprintf(stderr,"%d  id:%s ip:%s cseq:%s pids:",i, session[i].id, inet_ntoa(session[i].client_ip), session[i].cseq);
        for (j=0;j<s->npids;j++)
	    fprintf(stderr,"%d ",s->pid[j]);
	puts("\n");
    }
}

void add_pids(struct SESSION *s,char *pidstr)
{
    char *p,*ep;
    int pid;
    
    for(p=pidstr;*p;)
    {
	if (s->npids>=MAXPIDS) { fprintf(stderr,"MAXPIDS erreicht\n"); return; }
	pid=strtol(p,&ep,10);
	if (ep==p) break;
	s->pid[s->npids]=pid; s->npids++;
	if (*ep==',') { p=ep+1; continue; }
	break;
    }
}

void set_pids(struct SESSION *s,char *pidstr)
{
    s->npids=0;
    add_pids(s,pidstr);
}

int open_udp(char *srvip,int port)
{
    struct sockaddr_in sin={AF_INET};
    struct hostent *phent;
    int s;

    phent=gethostbyname(srvip);
    if (phent==NULL) { fprintf(stderr,"gethostbyname(%s) failed\n",srvip); return -1;  }
    memmove(&sin.sin_addr,phent->h_addr,sizeof(sin.sin_addr));
    sin.sin_port=htons(port);

    if((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) { perror("socket failed"); return -1; }

    if(bind(s,(struct sockaddr*)&sin,sizeof(sin))==-1) { perror("udp socket bind failed"); return -1; }

    return s;
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

int connect_server()
{
    struct hostent *phent;
    struct servent *pservent;
    static struct sockaddr_in sa={AF_INET};
    static int firstrun=1;
    int s,portn;

    if (firstrun)
    {
        phent=gethostbyname(target);
        if (phent==NULL) { fprintf(stderr,"gethostbyname failed\n"); return -1;  }
        portn=atoi(port);
        if (port==0)
        {
            pservent=getservbyname(port,"tcp");
            if (pservent==NULL)
            { fprintf(stderr,"getservbyname() failed\n"); return -1; }
            portn=ntohs(pservent->s_port);
        }
        memmove(&sa.sin_addr,phent->h_addr,sizeof(sa.sin_addr));
        sa.sin_port=htons(portn);
        firstrun=0;
    }

    s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (s<0) { fprintf(stderr,"socket() failed\n"); return -1;  }
    if (connect(s,(struct sockaddr *)&sa,sizeof(sa))<0) 
    {
        fprintf(stderr,"connect failed (%s)\n",strerror(errno));
        return -1;
    }
    return s;
}

char *reasonstr[]={"OK","Timeout","Hangup","Request to long","Server not reachable","Request Translation Failed"};

void dropconnection(int n,int reason)
{
    if (lfd_m[n].type==f_udprcv) { fprintf(stderr,"dropconnection not allowed for udp session\n"); return; }

    if (lfd_m[n].type==f_server) n--;	// drop client and server connection, select client first
    if (debug)
    {
        fprintf(stderr,"%ld disconnect from %s: %s (%d/%d)\n",now,inet_ntoa(lfd_m[n].client_ip),reasonstr[reason],n,nfd);
    }
    close(lfd[n].fd);
    remove_lfd(n);
    close(lfd[n+1].fd);
    remove_lfd(n+1);
}

int req_complete(char *s)
{
    if(strstr(s,"\r\n\r\n")) return TRUE;
    return FALSE;
}

int rsp_complete(char *s)
{
    char *p, *q;
    if((p=strstr(s,"\r\n\r\n")))
    {
    	int len;
        if ((q=strcasestr(s,"Content-Length: ")) && sscanf(q+16,"%d\r\n",&len)==1)
                return strlen(p+4)>=len;
        return TRUE;
    }
    return FALSE;
}

void remove_header(char *line[],int r)
{
    int i;

    free(line[r]);
    for(i=r;*line[i];i++) line[i]=line[i+1];
}

int search_header(char *line[],char *needle,int start)
{
    int i,len;
    len=strlen(needle);

    for(i=start;*line[i];i++)
    {
	if(!strncasecmp(line[i],needle,len) && (line[i])[len]==':')  // found
		return i;
    }
    return -1;
}

// get header content from request/response headers by header name
// headers are unchanged
char *get_header(char *line[],char *needle)
{
    int i;
    char *p;

    i=search_header(line,needle,1);
    if (i<=0) return NULL;

    for(p=line[i]+strlen(needle)+1;*p && *p==' ';p++);
    return p;
}

// extracts session id from request headers without additional parameters
// string gets overwriten with every call, so copy it if needed later
// original header is unchanged
char *get_sessionid(char *line[])
{
    static char sessionid[50];
    char *h;
    int i;

    h=get_header(line,"session");
    if (!h) return NULL;

    for(i=0;i<49 && h[i] && h[i]!=' ' && h[i]!=';';i++) sessionid[i]=h[i];
    sessionid[i]=0;
 
    return sessionid;
}

int udp_recv_port=15000;

int start_udp_proxy(struct SESSION *s,struct in_addr client_ip,int client_port)
{
    int fd;

    if (nfd>=MAXOPENFDS-2) { fprintf(stderr,"Maxumum FDS reached"); return -1; }

    s->recv_port=udp_recv_port;

    fd=open_udp(srvip,udp_recv_port);
    if (fd<0) return -1;
    s->udp_fd=fd;

    lfd[nfd].fd=fd;
    lfd[nfd].events=POLLIN|POLLPRI;
    lfd[nfd].revents=0;
    lfd_m[nfd].type=f_udprcv;
    lfd_m[nfd].sessionpointer=s;
    lfd_m[nfd].saddr.sin_family=AF_INET;
    lfd_m[nfd].saddr.sin_addr=client_ip;
    lfd_m[nfd].saddr.sin_port=ntohs(client_port);
    lfd_m[nfd].lastact=now;
    lfd_m[nfd].deleted=FALSE;
    nfd++; 

    fd=open_udp(srvip,udp_recv_port+1);
    if (fd<0) return -1;
    s->udp2_fd=fd;

    lfd[nfd].fd=fd;
    lfd[nfd].events=POLLIN|POLLPRI;
    lfd[nfd].revents=0;
    lfd_m[nfd].type=f_udprcv;
    lfd_m[nfd].sessionpointer=s;
    lfd_m[nfd].saddr.sin_family=AF_INET;
    lfd_m[nfd].saddr.sin_addr=client_ip;
    lfd_m[nfd].saddr.sin_port=ntohs(client_port+1);
    lfd_m[nfd].lastact=now;
    lfd_m[nfd].deleted=FALSE;
    nfd++; 

    udp_recv_port+=2;
    return 1;
}

// Sample SETUP Transport
//   Example C>S: Transport:RTP/AVP;unicast;client_port=5000-5001
//   Example S>C: Transport: RTP/AVP;unicast;destination=192.168.45.1;source=192.168.45.40;client_port=5000-5001;server_port=6976-6977

int handle_setup(char *line[],struct in_addr client_ip)
{
    int i,client_port=-1;
    char *sessionid,*part,*cseq;
    struct SESSION *s;
    char newtransport[1000],parameter[50];

	// ---------------------------------------- lookup session
    sessionid=get_sessionid(line);
    s=get_session(sessionid);
    if (!s) 
    {
        cseq=get_header(line,"cseq");
    	if (cseq==NULL) return -1;
        s=get_session_by_cseq(cseq);
	if (!s) s=start_session(client_ip,cseq);
    }
    if (!s) return -1;

	// transform transport header
    for(i=1;;) // search transport headers
    {
	i=search_header(line,"transport",i);
        if (i<0) break; 

	if (strcasestr(line[i],"multicast")) remove_header(line,i);
	else 
	{
    	    *newtransport=0;
	    strcpy(newtransport,"Transport:");
	    part=strtok(line[i]+10,";");
	    while(part)
	    {
		if (!strncasecmp(part,"client_port=",12)) 
		{
		    if (debug>1) fprintf(stderr,"Found client_port: %s\n",part);
		    client_port=atol(part+12); // takes first port number from range 
					       // sat>ip uses two ports: ts and tuner_info

		    if (s->client_port<0) { s->client_port=client_port; }
		    if (s->recv_port<0) start_udp_proxy(s,s->client_ip,client_port);
		    sprintf(parameter,"client_port=%d-%d",s->recv_port,s->recv_port+1);

		    strcat(newtransport,parameter); strcat(newtransport,";"); 
		}
		else { strcat(newtransport,part); strcat(newtransport,";"); }
		part=strtok(NULL,";");
	    }
	    free(line[i]);
		// Some servers don't like trailing ";" it seems, so remove it
	    if ((part=strrchr(newtransport,';'))) *part=0;
	    line[i]=strdup(newtransport);
	    i++;
    	}
    }

    return 1;
}

void handle_play(char *line[])
{
    char *id,*p,*qs,*att;
    struct SESSION *s;
	// get session header
    id=get_sessionid(line);
    if (!(s=get_session(id))) return;

    if (s && s->cseq)  // remove old cseq when session is allready set
    { free(s->cseq); s->cseq=NULL; }

    // Sample Play Requests:  
    //	PLAY rtsp://192.168.0.1:554/stream=12?pids=0,18,20 RTSP/1.0
    //	PLAY rtsp://192.168.0.1:554/stream=12?addpids=1028 RTSP/1.0


    for(p=line[0];*p && *p!='?';p++);
    
    if (!*p) return;  // PLAY request without QS ?

    qs=strdup(p+1);
    if (!qs) { perror("out of memory"); exit(1); }

    att=strtok(qs,"&");
    while (att)
    {
		// TODO: we should use the streamid in SETUP Response and PLAY Request for multi stream sessions
	if (!strncmp(att,"pids=",5)) set_pids(s,att+5);
	if (!strncmp(att,"addpids=",8)) add_pids(s,att+8);
    	att=strtok(NULL,"&");
    }
}

void handle_teardown(char *line[])
{
    char *id;
	// get session header
    id=get_sessionid(line);
    if (id) remove_session(id);
}

char *replace_str(char *s,char *srch, char *repl, char *out, int cbout)
{
    char *p, *q=out;
    int iLen1=strlen(srch),iLen2=strlen(repl);
    
    if (!(p=strstr(s,srch))) strncpy(out,s,cbout);
    else {
    	while (p) {
	    strncpy(q,s,p-s);
	    q+=(p-s);
	    p+=iLen1;
	    strncpy(q,repl,iLen2);
	    q+=iLen2;
	    s=p;
	    p=strstr(s,srch);
	}
	strcpy(q,s);
    }
    return out;
}

char *translate_describe(char *s,int li,char *out,int cbout)
{
    return replace_str(s,target,lfd_m[li].srvip,out,cbout);
}

char *translate_request(char *s,int li)
{
    static char out[MAXREQUESTLEN+30];
    char *line[100],*thisline;
    regex_t top_regex;
    regmatch_t match[5];
    int i,ln,ret;
    char *p;

    *out=0;

    for (ln=0,p=s;ln<=99;ln++)		// split request up into lines
    {
        thisline=p;
	for (;*p && *p!='\r' && *p!='\n';p++);
	if (!*p) break;
	*p=0;
        line[ln]=strdup(thisline);
	p++;
	if (*p=='\r' || *p=='\n') p++;
    }
    if (ln>99) { fprintf(stderr,"Too many header header lines in request\n"); return NULL; }

			// compile all regex rules
    ret=regcomp(&top_regex,"^.* rtsp://([0-9.]+)[:/].*",REG_EXTENDED);	// change target IP in rtsp url
    if (ret) { fprintf(stderr,"compiling regex failed\n"); return NULL; }

    ret=regexec(&top_regex,line[0],5,match,0);
    if (!ret) // match
    {
	if (match[1].rm_so>0)
	{
	    strncpy(out,line[0],match[1].rm_so);
	    out[match[1].rm_so]=0;
	    strcat(out,target);
	    strcat(out,line[0]+match[1].rm_eo);
    	    strcat(out,"\r\n");
	    strncpy(lfd_m[li].srvip,line[0]+match[1].rm_so,match[1].rm_eo-match[1].rm_so);
	}
    }
    else { strcpy(out,line[0]); }

    if (!strncmp(line[0],"SETUP ",6)) 
	if (handle_setup(line,lfd_m[li].client_ip)<0) return NULL; 

    if (!strncmp(line[0],"PLAY ",5)) handle_play(line);
    if (!strncmp(line[0],"TEARDOWN ",9)) handle_teardown(line);

    free(line[0]);
    for (i=1;;i++)
    {
	strcat(out,line[i]);
        strcat(out,"\r\n");
	if (!*(line[i])) break;
        free(line[i]);
    }
    free(line[i]);

    regfree(&top_regex);
    return *out?out:s;
}

char *translate_response(char *s_in, int li)
{
    char *cseq,*sessionid;
    char *line[100],buf[MAXREQUESTLEN];
    static char out[MAXREQUESTLEN+30];
    int ln,i;
    char *p,*s,*q;
    struct SESSION *sess=NULL;

    s=strdup(s_in);
    if(!s) { perror("out of memory"); exit(1); }

    for (ln=0,p=s;*p && ln<=99;ln++)		// split request up into lines
    {
        line[ln]=p;
	for (;*p && *p!='\r' && *p!='\n';p++);
	if (!*p) continue;
	*p=0;
	p++;
	if (*p=='\r' || *p=='\n') p++;
    }
    if (ln>99) { fprintf(stderr,"Too many header header lines in response\n"); free(s); return NULL; }

    cseq=get_header(line,"cseq");
    sessionid=get_sessionid(line);

    if (cseq && sessionid)
    {
	if (!(sess=get_session(sessionid)))  			// new session
	{
	    sess=get_session_by_cseq(cseq);   		// search corresponding SETUP request
	    if (sess) sess->id=strdup(sessionid); 	// set sessionid
	}
    }

    if (get_header(line,"content-length") && (p=strstr(s_in,"\r\n\r\n")))
    {
	char body[MAXREQUESTLEN+30];

	translate_describe(p+4,li-1,body,sizeof(body));
        for(*out=0,i=0,q=out;i<ln&&*line[i];i++)
        {
    	    if(!strncasecmp(line[i],"content-length:",15))
		q+=sprintf(q,"Content-Length: %d\r\n", strlen(body));
	    else 
		q+=sprintf(q,"%s\r\n",translate_describe(line[i],li-1,buf,sizeof(buf)));
	}
	q+=sprintf(q,"\r\n%s",body);
    }
    else
    {
	*out=0;
        for(i=0,q=out;i<ln;i++)
        {
    	    if(sess && !strncasecmp(line[i],"transport:",10))
    	    {
		q+=sprintf(q,"Transport: RTP/AVP;unicast;destination=%s;source=%s;"
		    "client_port=%d-%d;server_port=%d-%d\r\n",
		    inet_ntoa(sess->client_ip),lfd_m[li-1].srvip,sess->client_port,sess->client_port+1,
		    sess->recv_port,sess->recv_port+1);
	    }
	    else q+=sprintf(q,"%s\r\n",translate_describe(line[i],li-1,buf,sizeof(buf)));
	}
    }
    free(s);
    return out;
}

void poll_loop(int accsock)
{
    int i,nret,newfd,len;
    time_t lastcollect;
    struct sockaddr_in sin;
    socklen_t sinlen;
    long long nc=0; // number of connections
    char *translated;

    time(&now);
    lfd[0].fd=accsock;
    lfd[0].events=POLLIN|POLLPRI;
    lfd[0].revents=0;
    lfd_m[0].lastact=now;
    lfd_m[0].type=f_accept;
    lfd_m[0].deleted=FALSE;
    nfd=1;

    lastcollect=now;

    while (1)
    {
        nret=poll(lfd,nfd,2000);
        time(&now);
	// if (debug>1) dump_sessions();
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
                if (lfd[0].revents&POLLIN && nfd>=MAXOPENFDS-1) 
                { nret--; fprintf(stderr,"maximum session number reached (%d)\n",MAXOPENFDS); }
                if (lfd[0].revents&POLLIN && nfd<MAXOPENFDS-1)            // new connection coming in
                {
                    nret--;
		    sinlen=sizeof(struct sockaddr_in);
                    newfd=accept(accsock,(struct sockaddr *)&sin, &sinlen);
                    if (newfd<0) fprintf(stderr,"accept socket failed %s\n",strerror(errno));
                    else
                    {
                        lfd[nfd].fd=newfd;
                        lfd[nfd].events=POLLIN|POLLPRI;
                        lfd[nfd].revents=0;
			lfd_m[nfd].type=f_client;
			lfd_m[nfd].inbuf_offset=0;
                        lfd_m[nfd].lastact=now;
                        lfd_m[nfd].client_ip=sin.sin_addr;
                        lfd_m[nfd].deleted=FALSE;
                        nfd++; nc++;

			newfd=connect_server();	 	// connect to server immediately
                        if (newfd<0)
			{
			    fprintf(stderr,"connect to server failed %s\n",strerror(errno));
			    nfd--;
			    close(lfd[nfd].fd);
			}
			else
			{
			    lfd[nfd].fd=newfd;		 // connect to server immediately
			    lfd[nfd].events=POLLIN|POLLPRI;
			    lfd[nfd].revents=0;
                            lfd_m[nfd].lastact=now;
			    lfd_m[nfd].type=f_server;
			    lfd_m[nfd].inbuf_offset=0;
                            lfd_m[nfd].deleted=FALSE;
			    nfd++; 
			}
                    }
                }
                for(i=1;i<nfd && nret>0;i++)                     // data coming in ?
                {
                    if (lfd[i].revents) nret--;
                    if (lfd[i].revents&(POLLHUP|POLLERR|POLLNVAL)) { fprintf(stderr,"HUP/ERR/NVAL %d\n",lfd[i].fd); dropconnection(i,2); }
                    else if (lfd[i].revents&POLLIN) 
                    {
			lfd[i].revents=0;
			if (lfd_m[i].type==f_udprcv)
			{
    			    len = recv(lfd[i].fd, lfd_m[i].inbuf, MAXREQUESTLEN, 0);
                	    if (debug>2) fprintf(stderr,"----------------------- udp bytes:%d\n",len);
			    sendto(lfd[i].fd,lfd_m[i].inbuf,len,0,&lfd_m[i].saddr,sizeof(lfd_m[i].saddr));
			    continue;
			}

                        len=read(lfd[i].fd,lfd_m[i].inbuf+lfd_m[i].inbuf_offset,MAXREQUESTLEN-lfd_m[i].inbuf_offset);
                        if (len<=0) dropconnection(i,2);
                        else
                        {
                            lfd_m[i].lastact=now;
			    if (lfd_m[i].type==f_client)
                            { 							// collect data up to a full rtsp request
				lfd_m[i].inbuf[lfd_m[i].inbuf_offset+len]=0;
			    	if (req_complete(lfd_m[i].inbuf))
			    	{
			            if (debug>1) fprintf(stderr,"------------------- new req\n%s------------------\n",lfd_m[i].inbuf);
				    translated=translate_request(lfd_m[i].inbuf,i);
				    if (!translated) { dropconnection(i,5); continue; }
			            if (debug>1) fprintf(stderr,"------------------- converted req\n%s------------\n",translated);
                            	    write(lfd[i+1].fd,translated,strlen(translated));
				    lfd_m[i].inbuf_offset=0;
			        } else
			        {
				    lfd_m[i].inbuf_offset+=len;
                                    if (lfd_m[i].inbuf_offset>=MAXREQUESTLEN-1) dropconnection(i,3);
			        }
			    }
			    else if (lfd_m[i].type==f_server)
			    {
				if (!rsp_complete(lfd_m[i].inbuf))
				{
				    lfd_m[i].inbuf_offset+=len;
				    if (lfd_m[i].inbuf_offset>=MAXREQUESTLEN-1) dropconnection(i,3);
				}
				else if (!strncmp(lfd_m[i].inbuf,"RTSP/1.0 200",12))
				{
				    lfd_m[i].inbuf[lfd_m[i].inbuf_offset+len]=0;
				    lfd_m[i].inbuf_offset=0;
				    translated=translate_response(lfd_m[i].inbuf,i);
				    if (!translated) { dropconnection(i,5); continue; }
				    if (debug>1) fprintf(stderr,"----------------------- translated res\n%s",translated);
				    write(lfd[i-1].fd,translated,strlen(translated));
				}
				else
				{
				    if (debug>1)
				    {
				        lfd_m[i].inbuf[len]=0;
				        fprintf(stderr,"----------------------- res\n%s",lfd_m[i].inbuf);
				    }
				    lfd_m[i].inbuf_offset=0;
				    write(lfd[i-1].fd,lfd_m[i].inbuf,len);
				}
			    } else fprintf(stderr,"unknown lfd type. ignored");
			}
                    }
                }
        }
	// TODO: we need a better idle timeout for tcp and udp connections based on the session
        // if (lastcollect<now-10)
        // {
	   // dump_sessions();
           // if (debug>2) fprintf(stderr,"dropping sessions older than %d seconds\n",idletimeout);
           // for (i=1;i<nfd;i++) 
                // if (lfd_m[i].type==f_client && !lfd_m[i].deleted && lfd_m[i].lastact+idletimeout<now) 
		   // dropconnection(i,1);  // marks fds as removed, clean up is done later
           // lastcollect=now;
        // }
	cleanup_lfd();
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

