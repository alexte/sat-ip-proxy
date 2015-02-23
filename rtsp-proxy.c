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

#define TRUE 1
#define FALSE 0

char *prg;
int debug=0;
char *port="554";
char *srvip="0.0.0.0";
char *target;
int idletimeout=120;
int nr_sessions=0;

struct {
    char *id;
    struct in_addr srcip;
    int client_port;
    int lastuse;
} session[MAXSESSIONS];

void usage()
{
    fprintf(stderr,"usage: %s [-d] [-i <srvip>] [-p <port>] <target>\n"
	           "    srvip: ip to listen to (default 0.0.0.0 = any)\n"
		   "    port: tcp port to listen and connect to rtsp (default 554)\n",prg);
}

int add_session(char *id,struct in_addr srcip,int client_port)
{
    int n=nr_sessions;
    char *p;

    for(p=id;*p && *p==' ';p++);
    if (n>=MAXSESSIONS) return -1;
    nr_sessions++;
    session[n].id=strdup(p);
    if (!session[n].id) return -1;
    session[n].srcip=srcip;
    session[n].client_port=client_port;
    return n;
}

void remove_session(char *id)
{
    int i;
    char *p;

    for(p=id;*p && *p==' ';p++);
    for (i=0;i<nr_sessions;i++)
	if (!strcmp(session[i].id,p)) break;
    if (i>=nr_sessions) return;

    free(session[i].id);
    nr_sessions--;
    for (;i<nr_sessions-1;i++)
	session[i]=session[i+1];
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

struct pollfd lfd[MAXOPENFDS];
char inbuf[MAXOPENFDS][MAXREQUESTLEN+1];
int inbuf_offset[MAXOPENFDS];
time_t lastact[MAXOPENFDS];
int is_client[MAXOPENFDS];
struct in_addr srcip[MAXOPENFDS];
int nfd;

char *reasonstr[]={"OK","Timeout","Hangup","Request to long","Server not reachable"};

void dropconnection(int n,int reason)
{
    int i;
    time_t now;

    if (!is_client[n]) n--;	// drop client and server connection, select client first
    if (debug)
    {
    	time(&now);
        fprintf(stderr,"%ld disconnect from %s: %s (%d/%d)\n",now,inet_ntoa(srcip[n]),reasonstr[reason],n,nfd);
    }
    close(lfd[n].fd);
    close(lfd[n+1].fd);
    for(i=n;i<nfd;i++) lfd[i]=lfd[i+2];
    nfd--; nfd--;
}

int req_complete(char *s)
{
    if(strstr(s,"\r\n\r\n")) return TRUE;
    return FALSE;
}

void remove_header(char *line[],int r)
{
    int i;

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

int new_udpport()     // TODO: new ports per session
{
    return 15000;
}

// Sample SETUP Transport
//   Example C>S: Transport:RTP/AVP;unicast;client_port=5000-5001
//   Example S>C: Transport: RTP/AVP;unicast;destination=192.168.45.1;source=192.168.45.40;client_port=5000-5001;server_port=6976-6977

int handle_setup(char *line[],struct in_addr srcip)
{
    int session,i,port,client_port=-1;
    char *part;
    char newtransport[1000],parameter[50];
	// transform transport header
    for(i=1;;) // search transport headers
    {
	i=search_header(line,"transport",i);
        if (i>0) 
        {
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
						   // BTW what's the range for ?
			port=new_udpport();
			sprintf(parameter,"client_port=%d",port);
		        strcat(newtransport,parameter); strcat(newtransport,";"); 
		    }
		    else { strcat(newtransport,part); strcat(newtransport,";"); }
		    part=strtok(NULL,";");
		}
		strcpy(line[i],newtransport);
		i++;
	    }
    	} 
	else break;
    }
    i=search_header(line,"session",1);

    if (client_port<0) return -1;
    if ((session=add_session(line[i]+8,srcip,client_port))<0) return -1;

	// setup udp proxy
    return session;
}

void handle_teardown(char *line[])
{
    int i;
	// get session header
    i=search_header(line,"Session",1);
    if (i>0) 
    {
	remove_session(line[i]+8);
    }
}

char *translate_request(char *s,struct in_addr srcip)
{
    static char out[MAXREQUESTLEN+30];
    char *line[100];
    regex_t top_regex;
    regmatch_t match[5];
    int i,ln,ret;
    char *p;

    *out=0;

    for (ln=0,p=s;ln<=99;ln++)		// split request up into lines
    {
        line[ln]=p;
	for (;*p && *p!='\r' && *p!='\n';p++);
	if (!*p) break;
	*p=0;
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
	}
    }
    else { fprintf(stderr,"Top request line match failed\n"); return NULL; }

    if (!strncmp(line[0],"SETUP ",5)) 
	if (handle_setup(line,srcip)<0) return NULL; 
    if (!strncmp(line[0],"TEARDOWN ",5)) 
	handle_teardown(line);

    for (i=1;;i++)
    {
	strcat(out,line[i]);
        strcat(out,"\r\n");
	if (!*(line[i])) break;
    }

    regfree(&top_regex);
    return *out?out:s;
}

void poll_loop(int accsock)
{
    int i,nret,newfd,len;
    time_t now,lastcollect;
    struct sockaddr_in sin;
    socklen_t sinlen;
    long long nc=0; // number of connections
    char *converted_request;

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
			is_client[nfd]=TRUE;
			inbuf_offset[nfd]=0;
                        lastact[nfd]=now;
                        srcip[nfd]=sin.sin_addr;
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
                            lastact[nfd]=now;
			    is_client[nfd]=FALSE;
			    inbuf_offset[nfd]=0;
			    nfd++; 
			}
                    }
                }
                for(i=1;i<nfd && nret>0;i++)                     // data coming in ?
                {
                    if (lfd[i].revents) nret--;
                    if (lfd[i].revents&POLLHUP) dropconnection(i,2);
                    if (lfd[i].revents&POLLERR) dropconnection(i,2);
                    if (lfd[i].revents&POLLNVAL) dropconnection(i,2);
                    if (lfd[i].revents&POLLIN) 
                    {
                        len=read(lfd[i].fd,inbuf[i]+inbuf_offset[i],MAXREQUESTLEN-inbuf_offset[i]);
                        if (len<=0) dropconnection(i,2);
                        else
                        {
                            lastact[i]=now;
			    if (!is_client[i])
			    {
				write(lfd[i-1].fd,inbuf[i],len);
				if (debug>1)
				{
				    inbuf[i][len]=0;
				    fprintf(stderr,"----------------------- res\n%s",inbuf[i]);
				}
	 		    }
			    else
                            { 							// collect data up to a full rtsp request
				inbuf[i][inbuf_offset[i]+len]=0;
			    	if (req_complete(inbuf[i]))
			    	{
			            if (debug>1) fprintf(stderr,"------------------- new req\n%s",inbuf[i]);
				    converted_request=translate_request(inbuf[i],srcip[i]);
				    if (!converted_request) { dropconnection(i,3); continue; }
			            if (debug>1) fprintf(stderr,"------------------- converted req\n%s",converted_request);
                            	    write(lfd[i+1].fd,converted_request,strlen(converted_request));
				    inbuf_offset[i]=0;
			        } else
			        {
				    inbuf_offset[i]+=len;
                                    if (inbuf_offset[i]>=MAXREQUESTLEN-1) dropconnection(i,3);
			        }
			    }
                        }
                    }
                }
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

