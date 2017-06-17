/* 
 * This file is part of minecraftd.

 * minecraftd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * minecraftd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with minecraftd.  If not, see <http://www.gnu.org/licenses/>.
 */

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
#include <sys/file.h>
#include <pwd.h>
#include <grp.h>
#include <map>
#include <string>

// I suspect if this works.
// minutes_wasted_here > 40 * 60 is definitely true

#define WAIT_SEC 0
#define WAIT_USEC 500000

bool respawn = true;
std::map<std::string, std::string> global_config;
std::string socket_path;

int set_nonblocking(int fd);
size_t trim(char* out, size_t len, const char* str);
int daemonize(char* name, char* path, char* outfile, char* errfile, char* infile);
double microtime();
void reload(int sig);
void term(int sig);
void instructions(char** argv);
void send_cmd(char* cmd);
int _open(std::string path, int mode);
int _open(std::string path, int mode, int perm);
FILE* fopen(std::string path, char* mode);

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

// https://stackoverflow.com/a/122721/4869560
size_t trim(char* out, size_t len, const char* str) {
    if (len == 0)
        return 0;
    const char* end;
    size_t out_size;

    while (isspace((unsigned char)*str))
        str++;
    if (*str == 0) {
        *out = 0;
        return 1;
    }

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    end++;

    out_size = (end - str) < (len - 1) ? (end - str) : len - 1;
    memcpy(out, str, out_size);
    out[out_size] = 0;
    return out_size;
}

// https://stackoverflow.com/a/21419213
int daemonize(char* name, char* path, char* outfile, char* errfile, char* infile) {
    if (!path) {
        path = (char*)"/";
    }
    if (!name) {
        name = (char*)"medaemon";
    }
    if (!infile) {
        infile = (char*)"/dev/null";
    }
    if (!outfile) {
        outfile = (char*)"/dev/null";
    }
    if (!errfile) {
        errfile = (char*)"/dev/null";
    }

    pid_t child;
    // fork, detach from process group leader
    if ((child = fork()) < 0) { // failed fork
        fprintf(stderr, "error: failed fork\n");
        exit(EXIT_FAILURE);
    }
    if (child > 0) { // parent
        exit(EXIT_SUCCESS);
    }
    if (setsid() < 0) { // failed to become session leader
        fprintf(stderr, "error: failed setsid\n");
        exit(EXIT_FAILURE);
    }

    // catch/ignore signals
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    // fork second time
    if ((child = fork()) < 0) { // failed fork
        fprintf(stderr, "error: failed fork\n");
        exit(EXIT_FAILURE);
    }
    if (child > 0) { // parent
        exit(EXIT_SUCCESS);
    }
    // new file permissions
    umask(0);
    // change to path directory
    chdir(path);
    // Close all open file descriptors
    int fd;
    for (fd = sysconf(_SC_OPEN_MAX); fd >= 0; --fd) {
        close(fd);
    }

    // reopen stdin, stdout, stderr
    stdin = fopen(infile, "r");    // fd=0
    stdout = fopen(outfile, "w+"); // fd=1
    stderr = fopen(errfile, "w+"); // fd=2

    // open syslog
    openlog(name, LOG_PID, LOG_DAEMON);
    return (0);
}

double microtime() {
    double t;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000;
}

void reload(int sig) {
    // do reload
    syslog(LOG_NOTICE, "SIGHUP received, reloading");
    send_cmd((char*)"reload");
    return;
}

void term(int sig) {
    // stop server
    respawn = false;
    syslog(LOG_NOTICE, "SIGTERM received, terminating");
    send_cmd((char*)"stop");
    return;
}

void instructions(char** argv) {
    printf("minecraftd v0.1 by lkp111138\n\
Usage: %s -d|-k|-s|-e [argument]\n\
        -d          : Start minecraftd in daemon mode\n\
        -k          : Stop server\n\
        -s something: Say something as [Server]\n\
        -e command  : Execute command in the console\n\
        -f file     : Use this config file\n\
        -p port     : Server port, overrides server.properties\n\
Precedence of flags are same as above\n",
           argv[0]);
}

void send_cmd(char* cmd) {
    int fd;
    // open connection
    struct sockaddr_un addr;
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path + 1, global_config["data_dir"].c_str(), sizeof(addr.sun_path) - 1);
    printf("data_dir = %s\n", global_config["data_dir"].c_str());
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        printf("connect \\0%s: %m", addr.sun_path + 1);
        exit(1);
    }
    // send
    write(fd, cmd, strlen(cmd));
    // wait for output
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = WAIT_SEC;
    tv.tv_usec = WAIT_USEC;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    char buf[1025];
    set_nonblocking(fd);
    int read_count = 0;
    while (select(1 + fd, &readfds, NULL, NULL, &tv)) {
        memset(buf, 0, 1025);
        read_count = 0;
        while (read(fd, buf, 1024) > 0) {
            ++read_count;
            printf("%s", buf);
            memset(buf, 0, 1025);
        }
        if (!read_count) { // eof
            close(fd);
            return;
        }
        tv.tv_sec = WAIT_SEC;
        tv.tv_usec = WAIT_USEC;
    }
}

int _open(std::string path, int mode) {
    return open(path.c_str(), mode);
}

int _open(std::string path, int mode, int perm) {
    return open(path.c_str(), mode, perm);
}

FILE* fopen(std::string path, const char* mode) {
    return fopen(path.c_str(), mode);
}

int main(int argc, char** argv) {
    char* cvalue = NULL;
    char c = 0;
    int pflag = 0, dflag = 0;
    // before before start, parse config file
    // look for a -f flag in options
    char* config_file_path = (char*) "/etc/minecraftd/config.txt"; //default
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "-f") == 0){
            config_file_path = argv[1 + i];
        }
    }
    FILE* config = fopen(config_file_path, "a+");
    if (config == NULL) {
        printf("Cannot open config file %s: %s\n", config_file_path, strerror(errno));
        return EXIT_FAILURE;
    }
    rewind(config);
    // some default global config
    global_config["java"] = "/usr/bin/java";
    global_config["minheap"] = "30";
    global_config["maxheap"] = "70";
    global_config["jarfile"] = "spigot-1.11.2.jar";
    global_config["restart_threshold"] = "60";
    global_config["user"] = "minecraft";
    global_config["group"] = "minecraft";
    global_config["data_dir"] = "/etc/minecraftd/data";
    global_config["port"] = "25565";

    // real parse code
    if (config != NULL) {
        // continue parse
        char* key = NULL;
        char* val = NULL;
        char real_key[33];
        char real_val[257];
        int status = 0;
        do {
            // free
            if (key != NULL) free(key);
            if (val != NULL) free(val);
            key = NULL;
            val = NULL;
            size_t s1 = 34, s2 = 258, s3 = 1025;
            status = getdelim(&key, &s1, '=', config);
            if (status > 33 || status < 0) {
                if (status < 0) break; //end of config file
                printf("Key %s is too long (%d, %d)\n", key, status, errno);
                getdelim(&val, &s3, '\n', config); //consume the line
                continue;
            }
            key[status - 1] = 0; //remove `='
            status = getdelim(&val, &s2, '\n', config);
            if (status > 257 || status < 0) {
                if (status < 0) break;
                printf("Value %s is too long\n", val);
                continue;
            }
            val[status - 1] = 0; // remove newline
            trim(real_val, 257, val);
            trim(real_key, 33, key);
            // define config items here
            global_config[real_key] = real_val;
        } while(status != -1);
        fclose(config);
    } else {
        printf("Unable to open config file `%s': %m", config_file_path);
        return EXIT_FAILURE;
    }
    socket_path = "\0";
    socket_path += global_config["data_dir"];

    // before start, check process existed
    int _pidfile = _open(global_config["data_dir"] + "/pidfile", O_RDONLY, 0644);
    int server_running = 0; // 0: not runnng, 1: running, -ve: error
    int last_pid = 0;
    if (_pidfile == -1) {
        server_running = errno != ENOENT ? -errno : 0;
    } else {
        if (flock(_pidfile, LOCK_EX|LOCK_NB) == -1) {
            switch (errno) {
                case EWOULDBLOCK:
                    server_running = 1;
                    char pid_str[6];
                    read(_pidfile, pid_str, 5);
                    sscanf(pid_str, "%d", &last_pid);
                    break;
                default:
                    server_running = -errno;
            }
        } else {
            // server not running
            server_running = false;
        }
    }

    if (argc < 2) {
        instructions(argv);
        exit(EXIT_SUCCESS);
    }
    while ((c = getopt(argc, argv, "dks:e:f:p:")) != -1) {
        switch (c) {
        case 'd':
            if (server_running == 0) {
                dflag = true;
            } else {
		    if (server_running == 1) {
                printf("Server already running\n");
		    } else {
		        printf("Error: %s\n", strerror(-server_running));
    		}
            exit(EXIT_FAILURE);
        }
        break;
        case 'k':
            // kill server, send stop
            if (server_running == 0) {
                printf("Server isn't running\n");
                return EXIT_FAILURE;
            }
            if (server_running < 0) {
                printf("Error locking pidfile: %s", strerror(-server_running));
            }
            send_cmd((char*)"stop");
            // dont respawn!
            kill(last_pid, 15);
            return 0;
            break;
        case 's':
            // say something, send "say argument"
            if (server_running == 0) {
                printf("Server isn't running\n");
                return EXIT_FAILURE;
            }
            if (server_running < 0) {
                printf("Error locking pidfile: %s", strerror(-server_running));
            }            
            char buf[1025];
            snprintf(buf, 1024, "say %s", optarg);
            send_cmd(buf);
            return 0;
            break;
        case 'e':
            // send command, send argument
            if (server_running == 0) {
                printf("Server isn't running\n");
                return EXIT_FAILURE;
            }
            if (server_running < 0) {
                printf("Error locking pidfile: %s", strerror(-server_running));
            }            
            send_cmd(optarg);
            return 0;
            break;
        case 'f':
            // DO NOTHING
            break;
        case 'p':
            global_config["port"] = optarg;
            break;
        case '?':
            if (optopt == 'e')
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            else if (isprint(optopt))
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            instructions(argv);
            return EXIT_FAILURE;
        default:
            instructions(argv);
            return 0;
        }
    }
    // parse config file
    // no longer able to setgid after setuid. minutes_wasted_here=90;
    // setgid(25565);
    // setuid(25565);
    if (dflag) {
        // drop privileges
        if (global_config["group"][0] != 0) {
            errno = 0;
            struct group* grp = getgrnam(global_config["group"].c_str());
            if (grp != NULL) {
                if (setgid(grp->gr_gid) == -1) {
                    printf("Try `sudo");
                    for (int i = 0; i < argc; ++i) {
                        printf(" %s", argv[i]);
                    }
                    printf("'\n");
                    return EXIT_FAILURE;
                } 
                    
            } else {
                printf("An error occured while mapping group `%s'\n", global_config["group"].c_str());
                exit(EXIT_FAILURE);
            }
        } else {
            setgid(25565); //if failed to set then use original
        }
        if (global_config["user"][0] != 0) { 
            struct passwd* user = getpwnam(global_config["user"].c_str());
            if (user != NULL) {
                if (setuid(user->pw_uid) == -1) {
                    printf("Try `sudo");
                    for (int i = 0; i < argc; ++i) {
                        printf(" %s", argv[i]);
                    }
                    printf("'\n");
                    return EXIT_FAILURE;
                } 
            } else {
                printf("An error occured while mapping user `%s'\n", global_config["user"].c_str());
                exit(EXIT_FAILURE);
            }
        } else {
            setuid(25565);
        }

        FILE* pidfile = fopen(global_config["data_dir"] + "/pidfile", "w+");
        if (pidfile == NULL) {
            printf("Cannot open pidfile for writing, aborting (%m)\n");
            exit(1);
        }
        // start daemon
        int res;
        printf("minecraftd starting\n");
        if ((res = daemonize((char*)"minecraftd", (char*)"/etc/minecraftd/", NULL, NULL, NULL)) != 0) {
            fprintf(stderr, "Error: daemonize failed\n");
            exit(EXIT_FAILURE);
        }
        umask(0002);
        pid_t daemon_id = getpid();
        // write pid
        pidfile = fopen(global_config["data_dir"] + "/pidfile", "w+");
        if (pidfile == NULL) {
            syslog(LOG_ERR, "Cannot open pidfile");
            exit(1);
        }

        syslog(LOG_NOTICE, "Daemon starting");
        fprintf(pidfile, "%d", daemon_id);
        fclose(pidfile);
        // signal handlers
        signal(SIGHUP, reload);
        signal(SIGTERM, term);
        // create a socket at home
        int ipc_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (ipc_fd < 0) {
            syslog(LOG_ERR, "Failed to create IPC socket, %m");
            exit(1);
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path + 1, global_config["data_dir"].c_str(), sizeof(addr.sun_path) - 2);

        syslog(LOG_INFO, "binding to \\0%s", addr.sun_path + 1);
        if (-1 == bind(ipc_fd, (struct sockaddr*)&addr, sizeof(addr))) {
            syslog(LOG_ERR, "bind errno = %d", errno);
            exit(1);
        }
        if (-1 == listen(ipc_fd, 1)) { // what? you need more connections?
            syslog(LOG_ERR, "listen errno = %d", errno);
            exit(1);
        }
        // init fd_set
        struct timeval tv;
        tv.tv_sec = WAIT_SEC;
        tv.tv_usec = WAIT_USEC;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ipc_fd, &readfds);
        int _pidfile = _open(global_config["data_dir"] + "/pidfile", O_RDWR|O_CLOEXEC);
        if (_pidfile == -1) {
            syslog(LOG_ERR, "Cannot open pidfile");
            exit(EXIT_FAILURE);
        } else {
            if (flock(_pidfile, LOCK_EX|LOCK_NB) == -1) {
                syslog(LOG_ERR, "Cannot lock pidfile");
                exit(EXIT_FAILURE);
            }
        }
        syslog(LOG_NOTICE, "Daemon started");
        // start command
        // threads
        int num_thr = std::thread::hardware_concurrency();
        // ram
        struct sysinfo si;
        sysinfo(&si);
        long ram_mb = (si.totalram * si.mem_unit) >> 20;
        syslog(LOG_NOTICE, "system ram total %ldMB", ram_mb);
        int minheap = 0.3 * ram_mb;
        int maxheap = 0.7 * ram_mb;
        int restart_count = 0;
        pid_t server_pid;
        int child_stdin[2];
        int child_stdout[2];
        double restarts[5] = {0};
        // process stored configs
        int minheap_percent = 30;
        int maxheap_percent = 70;
        int restart_threshold = 60;
            sscanf(global_config["minheap"].c_str(), "%d", &minheap_percent);
            minheap = minheap_percent * ram_mb / 100;
            sscanf(global_config["maxheap"].c_str(), "%d", &maxheap_percent);
            maxheap = maxheap_percent * ram_mb / 100;
            sscanf(global_config["restart_threshold"].c_str(), "%d", &restart_threshold);
        do {
            syslog(LOG_NOTICE, "Starting server %d...", restart_count);
            restarts[restart_count] = microtime();
            // cd to data dir
            chdir(global_config["data_dir"].c_str());
            // why dont just allocate a pty?
            int master_pty_fd = getpt();
            if (master_pty_fd < 0) {
                syslog(LOG_ERR, "Unable to allocate pty, error %m");
                exit(EXIT_FAILURE);
            }
            if (grantpt(master_pty_fd) != 0) {
                syslog(LOG_ERR, "grantpt: %m");
                exit(EXIT_FAILURE);
            }
            if (unlockpt(master_pty_fd) != 0) {
                syslog(LOG_ERR, "unlockpt: %m");
                exit(EXIT_FAILURE);
            }
            // allocated, open slave side
            int slave_pty_fd = open(ptsname(master_pty_fd), O_RDWR);
            char args[3][32];
            sprintf(args[0], "-Xmx%dM", maxheap);
            sprintf(args[1], "-Xms%dM", minheap);
            sprintf(args[2], "-XX:ParallelGCThreads=%d", num_thr);
            syslog(LOG_NOTICE, "arg0=%s, arg1=%s, arg2=%s", args[0], args[1], args[2]);
            // https://stackoverflow.com/a/12839498
            set_nonblocking(master_pty_fd);
            server_pid = fork();
            if (server_pid == 0) {
                // child
                dup2(slave_pty_fd, STDIN_FILENO);   // stdin
                dup2(slave_pty_fd, STDOUT_FILENO); // stdout
                dup2(slave_pty_fd, STDERR_FILENO); // stderr
                close(master_pty_fd);
                close(slave_pty_fd);
            syslog(LOG_NOTICE, "child %d calling execl(), cmd %s %s %s -XX:+UseConcMarkSweepGC -XX:+CMSIncrementalPacing %s -Duser.timezone=Asia/Hong_Kong -Djline.terminal=jline.UnsupportedTerminal -jar %s -p %s", getpid(), global_config["java"].c_str(), args[0], args[1], args[2], global_config["jarfile"].c_str(), global_config["port"].c_str());
                // server_pid=getpid();
                int _s = execl(global_config["java"].c_str(), "java", args[0], args[1], "-XX:+UseConcMarkSweepGC", "-XX:+CMSIncrementalPacing", args[2], "-Duser.timezone=Asia/Hong_Kong", "-Djline.terminal=jline.UnsupportedTerminal", "-jar", global_config["jarfile"].c_str(), "-p", global_config["port"].c_str(), (char*) NULL);
                syslog(LOG_ERR, "child %d back from execl(), status %d, errno %d", getpid(), _s, errno);
                exit(1);
            } else {
                // parent
                // wait on socket instead
                // select with timeval=.5s
                char buf[1025];
                int server_status = 0;
                sleep(1); // wait for process spawn
                int _k = 0;
                int client = -1;
                close(slave_pty_fd);
                while (_k != -1 || server_status != ESRCH) {
                    // try kill server_pid with signal 0.
                    // and send command
                    int _k = kill(server_pid, 0);
                    server_status = errno;
                    if (_k == -1 && server_status == ESRCH) { // who fucking cares about errno when kill() says 0?
                        // minutes_wasted_here=90;
                        break;
                    }
                    tv.tv_sec = WAIT_SEC;
                    tv.tv_usec = WAIT_USEC;
                    FD_ZERO(&readfds);
                    FD_SET(ipc_fd, &readfds);
                    if (client > 0)
                        FD_SET(client, &readfds);
                    int maxfd = ipc_fd > client ? ipc_fd : client;
                    maxfd = maxfd > master_pty_fd ? maxfd : master_pty_fd;
                    FD_SET(master_pty_fd, &readfds);
                    if (select(maxfd + 1, &readfds, NULL, NULL, &tv) >
                            0) { // maxfd+1. minutes_wasted_here=60;
                        if (FD_ISSET(ipc_fd, &readfds)) { // info from server
                            struct sockaddr_in clientname;
                            int size = sizeof clientname;
                            client = accept(ipc_fd, (struct sockaddr*)&clientname,
                                            (socklen_t*)&size);
                        }
                        if (client > 0 && FD_ISSET(client, &readfds)) { // info from client
                            memset(buf, 0, 1025);
                            if (read(client, buf, 1024)) { // not eof
                                write(master_pty_fd, buf, strlen(buf));
                                write(master_pty_fd, "\n", 1);
                            } else {
                                close(client);
                                client = -1;
                            }
                        }
                        if (FD_ISSET(master_pty_fd, &readfds)) {
                            // send to client if exist
                            while (read(master_pty_fd, buf, 1024) > 0) {
                                if (client > 0)
                                    write(client, buf, strlen(buf));
                                memset(buf, 0, 1025);
                            }
                        }
                    } else {
                        if (client > 0) {
                            close(client);
                            client = -1;
                        }
                    }
                }
                // waitpid(server_pid, NULL, 0);
                syslog(LOG_NOTICE, "Server pid %d exited", server_pid);
            }

            restart_count %= 5;
            if (restarts[restart_count] - restarts[(restart_count + 1) % 5] < restart_threshold) {
                respawn = false;
                syslog(LOG_ERR, "Server keeps commiting suicide (restarted 5 times in %f sec)... Giving up!", restarts[restart_count] - restarts[(restart_count + 1) % 5]);
            }
            if (respawn) {
                ++restart_count;
                sleep(5);
            }
        } while (respawn);
        closelog();
        return (EXIT_SUCCESS);
    }
}
