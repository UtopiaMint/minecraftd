#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <iostream>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <csignal>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>
#include <thread>
#include <fcntl.h>
#include <cstring>
#include <netdb.h>
#include <sys/time.h>

#define WAIT_SEC 0
#define WAIT_USEC 500000

using namespace std;

bool respawn=true;

int set_nonblocking(int fd);
int daemonize(char* name, char* path, char* outfile, char* errfile, char* infile);
void reload(int sig);
void term(int sig);
void instructions(char** argv);
void send_cmd(char* cmd);

int set_nonblocking(int fd){
    int flags = fcntl(fd,F_GETFL,0);
    flags |= O_NONBLOCK;
    return fcntl(fd,F_SETFL,flags);
}

//https://stackoverflow.com/a/21419213
int daemonize(char* name, char* path, char* outfile, char* errfile, char* infile){
    if(!path) { path=(char*)"/"; }
    if(!name) { name=(char*)"medaemon"; }
    if(!infile) { infile=(char*)"/dev/null"; }
    if(!outfile) { outfile=(char*)"/dev/null"; }
    if(!errfile) { errfile=(char*)"/dev/null"; }
    //printf("%s %s %s %s\n",name,path,outfile,infile);
    pid_t child;
    //fork, detach from process group leader
    if( (child=fork())<0 ) { //failed fork
        fprintf(stderr,"error: failed fork\n");
        exit(EXIT_FAILURE);
    }
    if (child>0) { //parent
        exit(EXIT_SUCCESS);
    }
    if( setsid()<0 ) { //failed to become session leader
        fprintf(stderr,"error: failed setsid\n");
        exit(EXIT_FAILURE);
    }

    //catch/ignore signals
    signal(SIGCHLD,SIG_IGN);
    signal(SIGHUP,SIG_IGN);
    //fork second time
    if ( (child=fork())<0) { //failed fork
        fprintf(stderr,"error: failed fork\n");
        exit(EXIT_FAILURE);
    }
    if( child>0 ) { //parent
        exit(EXIT_SUCCESS);
    }
    //new file permissions
    umask(0);
    //change to path directory
    chdir(path);
    //Close all open file descriptors
    int fd;
    for( fd=sysconf(_SC_OPEN_MAX); fd>0; --fd){
        close(fd);
    }
    
    //reopen stdin, stdout, stderr
    stdin=fopen(infile,"r");   //fd=0
    stdout=fopen(outfile,"w+");  //fd=1
    stderr=fopen(errfile,"w+");  //fd=2
    
    //open syslog
    openlog(name,LOG_PID,LOG_DAEMON);
    return(0);
}

void reload(int sig){
    //do reload
    syslog(LOG_NOTICE, "SIGHUP received, reloading");
    send_cmd((char*) "reload");
    return;
}

void term(int sig){
    //stop server
    respawn=false;
    syslog(LOG_NOTICE, "SIGTERM received, terminating");
    //syslog(LOG_NOTICE, (string("echo ")+screen+" > test").c_str());
    send_cmd((char*) "stop");
    return;
}

void instructions(char** argv){
    printf("minecraftd v0.1 by lkp111138\n\
            Usage: %s -d|-k|-s|-e [argument]\n\
            -d          : Start minecraftd in daemon mode\n\
            -k          : Stop server\n\
            -s something: Say something as [Server]\n\
            -e command  : Execute command in the console\n\
            Precedence of flags are same as above\n", argv[0]);
}

void send_cmd(char* cmd){
    int fd;
    //open connection
    struct sockaddr_un addr;
    if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/minecraftdipc", sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect");
        exit(1);
    }
    //send
    write(fd, cmd, strlen(cmd));
    //wait for output
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec=WAIT_SEC;
    tv.tv_usec=WAIT_USEC;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    char buf[1025];
    set_nonblocking(fd);
    int read_count=0;
    while(select(1+fd, &readfds, NULL, NULL, &tv)){
        memset(buf, 0, 1025);
        read_count=0;
        while(read(fd, buf, 1024)>0){
            ++read_count;
            printf("%s", buf);
            memset(buf, 0, 1025);
        }
        if(!read_count){ //eof
            close(fd);
            return;
        }
        tv.tv_sec=WAIT_SEC;
        tv.tv_usec=WAIT_USEC;
    }
}

int main(int argc, char** argv){
    char* cvalue=NULL;
    char c;
    int pflag=0, dflag=0;
    if(geteuid()){
        printf("Try `sudo");
        for(int i=0; i<argc; ++i){
            printf(" %s", argv[i]);
        }
        printf("'\n");
        return 1;
    }
    //before start, check process existed
    FILE* _pidfile=fopen("/etc/minecraftd/pidfile", "r+");
    pid_t last_pid;
    bool server_running=false;
    if(_pidfile == NULL){
        switch(errno){
            case ENOENT:
                //no such file
                break;
            default:
                perror("fopen on pidfile");
                exit(1);
                break;
        }
    }else{
        fscanf(_pidfile, "%d", &last_pid);
        kill(last_pid, 0);
        fclose(_pidfile);
        server_running=(errno!=ESRCH);
    }

    while ((c = getopt (argc, argv, "dks:e:")) != -1){
        switch (c){
            case 'd':
                dflag=true;
                break;
            case 'k':
                //kill server, send stop
                if(!server_running){
                    printf("Server isn't running\n");
                    return 1;
                }
                send_cmd((char*) "stop");
                //dont respawn!
                kill(last_pid, 15);
                return 0;
                break;
            case 's':
                //say something, send "say argument"
                if(!server_running){
                    printf("Server isn't running\n");
                    return 1;
                }
                char buf[1025];
                snprintf(buf, 1024, "say %s", argv[2]);
                send_cmd(buf);
                return 0;
                break;
            case 'e':
                //send command, send argument
                if(!server_running){
                    printf("Server isn't running\n");
                    return 1;
                }
                send_cmd(argv[2]);
                break;
            case '?':
                if (optopt == 'c')
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint (optopt))
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                instructions(argv);
                return 1;
            default:
                instructions(argv);
                return 0;
        }
    }
    //printf ("pflag = %d, dflag = %d, evalue = %s\n", pflag, dflag, cvalue);

    for (int index = optind; index < argc; index++)
        printf ("Non-option argument %s\n", argv[index]);
    //return 0;
    if(dflag){
        //before start, check process existed
        FILE* pidfile=fopen("/etc/minecraftd/pidfile", "r+");
        pid_t daemon_id;
        if(pidfile == NULL){
            switch(errno){
                case ENOENT:
                    //no such file
                    break;
                default:
                    perror("fopen on pidfile");
                    exit(1);
                    break;
            }
        }else{
            pid_t daemon_id;
            fscanf(pidfile, "%d", &daemon_id);
            kill(daemon_id, 0);
            fclose(pidfile);
            if(errno==ESRCH){
                //ok process doesnt exist
                printf("Starting daemon\n");
            }else{
                //running, exit
                printf("Daemon already running, exiting\n");
                exit(1);
            }
        }
        pidfile=fopen("/etc/minecraftd/pidfile", "w+");
        if(pidfile == NULL){
            printf("Cannot open pidfile for writing, aborting\n");
            exit(1);
        }
        //start daemon
        int res;
        if( (res=daemonize((char*)"minecraftd",(char*)"/etc/minecraftd/",NULL,NULL,NULL)) != 0 ) {
            fprintf(stderr,"Error: daemonize failed\n");
            exit(EXIT_FAILURE);
        }
        umask(0113);
        daemon_id=getpid();
        //write pid
        pidfile=fopen("/etc/minecraftd/pidfile", "w+");
        if(pidfile == NULL){
            syslog(LOG_ERR, "Cannot open pidfile");
            exit(1);
        }

        syslog(LOG_NOTICE, "Daemon starting");
        fprintf(pidfile, "%d", daemon_id);
        fclose(pidfile);
        //signal handlers
        signal(SIGHUP, reload);
        signal(SIGTERM, term);
        //no longer root
        setuid(25565);
        //create a socket at home
        int ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if(ipc_fd<0){
            syslog(LOG_ERR, "Failed to create IPC socket");
            exit(1);
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/minecraftdipc", sizeof(addr.sun_path)-1);
        //unlink before binding
        unlink("/tmp/minecraftdipc");
        if(-1==bind(ipc_fd, (struct sockaddr*)&addr, sizeof(addr))){
            syslog(LOG_ERR, "bind errno=%d", errno);
            exit(1);
        }
        if(-1==listen(ipc_fd, 1)){ //what? you need more connections?
            syslog(LOG_ERR, "listen errno=%d", errno);
            exit(1);
        }
        //init fd_set
        struct timeval tv;
        tv.tv_sec=WAIT_SEC;
        tv.tv_usec=WAIT_USEC;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ipc_fd, &readfds);
        syslog(LOG_NOTICE, "Daemon started");
        //start command
        //threads
        int num_thr=thread::hardware_concurrency();
        //ram
        struct sysinfo si;
        sysinfo(&si);
        long ram_mb=(si.totalram*si.mem_unit)>>20;
        //char test[30];
        //sprintf(test, "ram total %dMB", ram);
        syslog(LOG_NOTICE, "system ram total %ldMB", ram_mb);
        int minheap=0.3*ram_mb;
        int maxheap=0.7*ram_mb;
        int restart_count=1;
        pid_t server_pid;
        int child_stdin[2];
        int child_stdout[2];
        do{
            syslog(LOG_NOTICE, "Starting server %d...", restart_count);
            //cd to data dir
            chdir("/etc/minecraftd/data/");
            //open a pipe
            if(pipe(child_stdin)<0){
                //error
                syslog(LOG_ERR, "Failed to create pipe");
                exit(1);
            }
            if(pipe(child_stdout)<0){
                //error
                close(child_stdin[0]);
                close(child_stdin[0]);
                syslog(LOG_ERR, "Failed to create pipe");
                exit(1);
            }
            char args[3][32];
            sprintf(args[0], "-Xmx%dM", maxheap);
            sprintf(args[1], "-Xms%dM", minheap);
            sprintf(args[2], "-XX:ParallelGCThreads=%d", num_thr);
            syslog(LOG_NOTICE, "arg0=%s, arg1=%s, arg2=%s", args[0], args[1], args[2]);
            //https://stackoverflow.com/a/12839498
            //set_nonblocking(child_stdin[0]);
            //set_nonblocking(child_stdin[1]);
            set_nonblocking(child_stdout[0]);
            set_nonblocking(child_stdout[1]);
            server_pid=fork();
            //syslog(LOG_NOTICE, "fork called at 266");
            if(server_pid==0){
                //child
                dup2(child_stdin[0], STDIN_FILENO); //stdin
                dup2(child_stdout[1], STDOUT_FILENO); //stdout
                dup2(child_stdout[1], STDERR_FILENO); //stderr
                close(child_stdin[0]);
                close(child_stdin[1]);
                close(child_stdout[0]);
                close(child_stdout[1]);
                syslog(LOG_NOTICE, "child %d calling execl()", getpid());
                //int _s=execl("/bin/cat", "cat", (char*) NULL);
                //server_pid=getpid();
                int _s=execl("/usr/bin/java", "java", args[0], args[1], "-XX:+UseConcMarkSweepGC", "-XX:+CMSIncrementalPacing", args[2], "-Duser.timezone=Asia/Hong_Kong", "-jar", "spigot-1.11.2.jar", (char*) NULL);
                syslog(LOG_ERR, "child %d back from execl(), status %d, errno %d", getpid(), _s, errno);
                exit(1);
            }else{
                //parent
                close(child_stdin[0]);
                close(child_stdout[1]);
                //wait on socket instead
                //select with timeval=1s
                char buf[1025];
                int server_status=0;
                sleep(1); //wait for process spawn
                int _k=0;
                int client=-1;
                while(_k!=-1 || server_status!=ESRCH){
                    //try kill server_pid with signal 0.
                    //and send command
                    int _k=kill(server_pid, 0);
                    server_status=errno;
                    //syslog(LOG_NOTICE, "kill(%d, 0) = %d, errno = %d", server_pid,_k,  errno);
                    if(_k==-1 && server_status==ESRCH){ //who fucking cares about errno when kill() says 0? minutes_wasted_here=90;
                        //sleep(10);
                        break;
                    }
                    tv.tv_sec=WAIT_SEC;
                    tv.tv_usec=WAIT_USEC;
                    //int client=-1;
                    FD_ZERO(&readfds);
                    FD_SET(ipc_fd, &readfds);
                    if(client>0) FD_SET(client, &readfds);
                    int maxfd=ipc_fd>client?ipc_fd:client;
                    //bool more_stdout=false;
                    maxfd=maxfd>child_stdout[0]?maxfd:child_stdout[0];
                    FD_SET(child_stdout[0], &readfds);
                    if(select(maxfd+1, &readfds, NULL, NULL, &tv)>0){ //maxfd+1. minutes_wasted_here=60;
                        if(FD_ISSET(ipc_fd, &readfds)){ //info from server
                            struct sockaddr_in clientname;
                            int size=sizeof clientname;
                            client=accept(ipc_fd, (struct sockaddr*) &clientname, (socklen_t*) &size);
                        }
                        if(client>0 && FD_ISSET(client, &readfds)){ //info from client
                            memset(buf, 0, 1025);
                            if(read(client, buf, 1024)){ //not eof
                                write(child_stdin[1], buf, strlen(buf));
                                write(child_stdin[1], "\n", 1);
                            }else{
                                close(client);
                                client=-1;
                            }
                        }
                        if(FD_ISSET(child_stdout[0], &readfds)){
                            //send to client if exist
                            while(read(child_stdout[0], buf, 1024)>0){
                                if(client>0) write(client, buf, strlen(buf));
                                memset(buf, 0, 1025);
                                //more_stdout=true;
                            }
                        }
                    }else{
                        //more_stdout=false;
                        if(client>0){
                            close(client);
                            client=-1;
                        }
                    }
                }
                //waitpid(server_pid, NULL, 0);
                syslog(LOG_NOTICE, "Server pid %d exited", server_pid);
            }
            close(child_stdin[1]);
            close(child_stdout[0]);
            
            ++restart_count;
            if(restart_count>5){
                respawn=false;
                syslog(LOG_ERR, "Server keeps commiting suicide... Giving up!");
            }
            if(respawn){
                sleep(5);
            }
        }while(respawn);
        closelog();
        return(EXIT_SUCCESS);
    }
}

