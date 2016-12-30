#define _GNU_SOURCE
#include "csiebox_server.h"
#include "csiebox_common.h"
#include "server_function.h"
#include "connect.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <sys/select.h>
#include <fts.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <sys/file.h>
#include <signal.h>

static int parse_arg(csiebox_server* server, int argc, char** argv);
static void handle_request(csiebox_server* server, int conn_fd);
static int get_account_info(
  csiebox_server* server,  const char* user, csiebox_account_info* info);
static void login(
  csiebox_server* server, int conn_fd, csiebox_protocol_login* login);
static void logout(csiebox_server* server, int conn_fd);
static char* get_user_homedir(
  csiebox_server* server, csiebox_client_info* info);
static int handle_download(csiebox_server *server,int conn_fd);

//for signal

static void signal_blockUSR1();
static void signal_unblockUSR1();
char FIFO_FILE[PATH_MAX];
char pid_file[PATH_MAX];;

void signal_handler(int);
void signal_handler_del(int);
struct sigaction act, act_del;
static void daemonize(char* run_path);



// for multithread programming
//
struct request
{
	csiebox_server* server;
	int conn_fd;
	struct request* next;
};

void* handle_req_loop(void*);
void add_req( pthread_mutex_t *mutex, pthread_cond_t *cond_t, const int *dim, struct request *request);
int check_thr( const int*);
struct request* set_request( int, csiebox_server*);
struct request* get_req();

struct request* req_queue = NULL;
struct request* last_req = NULL;
int req_num = 0;
int* thr_state;													// record the state of worker thread
int client_state[256];
int thr_state_size = 0;

pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t got_req = PTHREAD_COND_INITIALIZER;

#define DIR_S_FLAG (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)//permission you can use to create new file
#define REG_S_FLAG (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)//permission you can use to create new directory

//read config file, and start to listen
//=====================================
//				TODO
//You should add your code of initializing 
//thread pool here
//=====================================
void csiebox_server_init(
  csiebox_server** server, int argc, char** argv) {
  csiebox_server* tmp = (csiebox_server*)malloc(sizeof(csiebox_server));
  if (!tmp) {
    fprintf(stderr, "server malloc fail\n");
    return;
  }
  memset(tmp, 0, sizeof(csiebox_server));
  if (!parse_arg(tmp, argc, argv)) {
    fprintf(stderr, "Usage: %s [config file]\n", argv[0]);
    free(tmp);
    return;
  }
  int fd = server_start();
  if (fd < 0) {
    fprintf(stderr, "server fail\n");
    free(tmp);
    return;
  }
  tmp->client = (csiebox_client_info**)
      malloc(sizeof(csiebox_client_info*) * getdtablesize());
  if (!tmp->client) {
    fprintf(stderr, "client list malloc fail\n");
    close(fd);
    free(tmp);
    return;
  }
  memset(tmp->client, 0, sizeof(csiebox_client_info*) * getdtablesize());
  tmp->listen_fd = fd;
  *server = tmp;

	
	act.sa_handler = signal_handler;
	act_del.sa_handler = signal_handler_del;
	signal_blockUSR1();
	sigaction(SIGUSR1, &act, NULL);
	sigaction(SIGINT, &act_del, NULL);
	sigaction(SIGTERM, &act_del, NULL);


	// multithread programming
	pthread_t *threads = (pthread_t*)malloc(tmp->arg.thread_num * sizeof(pthread_t));
  int *thr_id = (int*)malloc(tmp->arg.thread_num * sizeof(int));

	for (int i = 0; i < tmp->arg.thread_num; i++){
		thr_id[i] = i;
	  pthread_create(&threads[i], NULL,handle_req_loop, (void*)&thr_id[i]);
	}
	sleep(2);			//wait for thread creation
}

//wait client to connect and handle requests from connected socket fd
int csiebox_server_run(csiebox_server* server) {
  int conn_fd, conn_len;
  struct sockaddr_in addr;
  fd_set read_set;
  int max_fd = -1;
  int i = 0;
	

	thr_state = (int*)malloc(server->arg.thread_num * sizeof(int));					  // free:1  not free:0
	thr_state_size = server->arg.thread_num;
	for ( int i = 0; i < server->arg.thread_num; ++i) thr_state[i] = 1;				// initialize the thread state
  
	//signal
	act.sa_handler = signal_handler;
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);
 
  FD_ZERO(&read_set);
  FD_SET(server->listen_fd,&read_set);
  max_fd = server->listen_fd;

  while (1) {
	select( max_fd+1, &read_set, NULL, NULL, NULL);
	if( FD_ISSET(server->listen_fd, &read_set))
	{
		memset(&addr, 0, sizeof(addr));
	    conn_len = 0;
	    // waiting client connect
	    conn_fd = accept(
	      server->listen_fd, (struct sockaddr*)&addr, (socklen_t*)&conn_len);
	    if (conn_fd < 0) {
			if (errno == ENFILE) {
	          fprintf(stderr, "out of file descriptor table\n");
	        } else if (errno == EAGAIN || errno == EINTR) {

	        } else {
	          fprintf(stderr, "accept err\n");
	          fprintf(stderr, "code: %s\n", strerror(errno));
	       }
		   FD_ZERO(&read_set);
		   FD_SET(server->listen_fd, &read_set);
		   max_fd = server->listen_fd;
		   for( i = 0; i < getdtablesize(); ++i )
		   {
			   if( !server->client[i])
					continue;
		   	   FD_SET(server->client[i]->conn_fd, &read_set);
			   max_fd = max_fd > server->client[i]->conn_fd ? max_fd : server->client[i]->conn_fd;
		   }
		   continue;
	    }
		handle_request(server, conn_fd);

	}
	else{
		for( i = 0; i < getdtablesize(); ++i )
		{
			if( !server->client[i])
				continue;
			if( FD_ISSET(server->client[i]->conn_fd, &read_set))
			{
				//=================================================
				//						TODO
				//You should modify this part of code so that main
				//thread can assign request to worker thread
				//=================================================
				//handle_request(server, server->client[i]->conn_fd);
				if(client_state[i] == 1) continue;
				else
				{
				struct request* a_request = set_request(server->client[i]->conn_fd, server);
				add_req(&req_mutex, &got_req, &(server->arg.thread_num), a_request);
				}
			}
		}
	}
	FD_ZERO(&read_set);
	FD_SET(server->listen_fd, &read_set);
	max_fd = server->listen_fd;
	for( i = 0; i < getdtablesize(); ++i )
	{
		if( !server->client[i])
			continue;
		FD_SET(server->client[i]->conn_fd, &read_set);
		max_fd = max_fd > server->client[i]->conn_fd ? max_fd : server->client[i]->conn_fd;
	}
    // handle request from connected socket fd
  }
	free(thr_state);
  return 1;
}

void csiebox_server_destroy(csiebox_server** server) {
  csiebox_server* tmp = *server;
  *server = 0;
  if (!tmp) {
    return;
  }
  close(tmp->listen_fd);
  int i = getdtablesize() - 1;
  for (; i >= 0; --i) {
    if (tmp->client[i]) {
      free(tmp->client[i]);
    }
  }
  free(tmp->client);
  free(tmp);
}

//read config file
//=============================
//			TODO
// You should add your code of reading thread_num from server.cfg here
//=============================
static int parse_arg(csiebox_server* server, int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    return 0;
  }
  FILE* file = fopen(argv[1], "r");
  if (!file) {
    return 0;
  }
  fprintf(stderr, "reading config...\n");
  size_t keysize = 20, valsize = 20;
  char* key = (char*)malloc(sizeof(char) * keysize);
  char* val = (char*)malloc(sizeof(char) * valsize);
  ssize_t keylen, vallen;
  int accept_config_total = 4;
  int accept_config[4] = {0, 0, 0, 0};
  while ((keylen = getdelim(&key, &keysize, '=', file) - 1) > 0) {
    key[keylen] = '\0';
    vallen = getline(&val, &valsize, file) - 1;
    val[vallen] = '\0';
    fprintf(stderr, "config (%d, %s)=(%d, %s)\n", keylen, key, vallen, val);
    if (strcmp("path", key) == 0) {
      if (vallen <= sizeof(server->arg.path)) {
        strncpy(server->arg.path, val, vallen);
        accept_config[0] = 1;
      }
    } else if (strcmp("account_path", key) == 0) {
      if (vallen <= sizeof(server->arg.account_path)) {
        strncpy(server->arg.account_path, val, vallen);
        accept_config[1] = 1;
      }
    } else if (strcmp("thread_num",key) == 0) {
      	server->arg.thread_num = (int) strtol(val, (char **)NULL, 10);
	 		 	accept_config[2] = 1;     	
		} else if (strcmp("run_path",key) == 0) {
				char run_path[PATH_MAX];
				strncpy(run_path, val, vallen);

				// damonize ?
				if (argc == 3 && strcmp(argv[2], "-d") == 0)
				{
					daemonize(run_path);
				}

				//Create the FIFO file
				snprintf(FIFO_FILE, PATH_MAX, "%s/csiebox_server.%d", run_path, getpid());
				if ( mkfifo(FIFO_FILE, 0777) == -1) fprintf(stderr, "FIFO file create error\n");
				fprintf(stderr, "FIFO file path: %s\n", FIFO_FILE);
				accept_config[3] = 1;
		}


  }
  free(key);
  free(val);
  fclose(file);
  int i, test = 1;
  for (i = 0; i < accept_config_total; ++i) {
    test = test & accept_config[i];
  }
  if (!test) {
    fprintf(stderr, "config error\n");
    return 0;
  }
  return 1;
}

//this is where the server handle requests, you should write your code here
static void handle_request(csiebox_server* server, int conn_fd) {
  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  if( !recv_message(conn_fd, &header, sizeof(header))){
    fprintf(stderr, "end of connection\n");
    logout(server, conn_fd);
    return;
  }
  if (header.req.magic != CSIEBOX_PROTOCOL_MAGIC_REQ) {
    return;
  }
  switch (header.req.op) {
    case CSIEBOX_PROTOCOL_OP_LOGIN:
      fprintf(stderr, "login\n");
      csiebox_protocol_login req;
      if (complete_message_with_header(conn_fd, &header, &req)) {
        login(server, conn_fd, &req);
		handle_download(server,conn_fd);
		csiebox_protocol_header sync_end;
		memset(&sync_end, 0, sizeof(csiebox_protocol_header));
		sync_end.req.magic = CSIEBOX_PROTOCOL_MAGIC_REQ;
		sync_end.req.op	  = CSIEBOX_PROTOCOL_OP_SYNC_END;
		send_message(conn_fd, &sync_end, sizeof(sync_end));
      }
      break;
    case CSIEBOX_PROTOCOL_OP_SYNC_META:
      fprintf(stderr, "sync meta\n");
      csiebox_protocol_meta meta;
      if (complete_message_with_header(conn_fd, &header, &meta)) {
        //====================
        //        TODO
		// You should add exclusive lock on file that is currenting synchronizing
        //====================
			server_sync_meta(meta, conn_fd, server);
						
      }
      break;
    case CSIEBOX_PROTOCOL_OP_SYNC_HARDLINK:
      fprintf(stderr, "sync hardlink\n");
      csiebox_protocol_hardlink hardlink;
      if (complete_message_with_header(conn_fd, &header, &hardlink)) {

		server_sync_hardlink( hardlink, conn_fd, server);
      }
      break;
    case CSIEBOX_PROTOCOL_OP_SYNC_END:
      fprintf(stderr, "sync end\n");
      csiebox_protocol_header end;

      break;
    case CSIEBOX_PROTOCOL_OP_RM:
      fprintf(stderr, "rm\n");
      csiebox_protocol_rm rm;
      if (complete_message_with_header(conn_fd, &header, &rm)) {

		server_rm(rm, conn_fd, server);
      }
      break;
    default:
      fprintf(stderr, "unknown op %x\n", header.req.op);
      break;
  }
}

//open account file to get account information
static int get_account_info(
  csiebox_server* server,  const char* user, csiebox_account_info* info) {
  FILE* file = fopen(server->arg.account_path, "r");
  if (!file) {
    return 0;
  }
  size_t buflen = 100;
  char* buf = (char*)malloc(sizeof(char) * buflen);
  memset(buf, 0, buflen);
  ssize_t len;
  int ret = 0;
  int line = 0;
  while ((len = getline(&buf, &buflen, file) - 1) > 0) {
    ++line;
    buf[len] = '\0';
    char* u = strtok(buf, ",");
    if (!u) {
      fprintf(stderr, "illegal form in account file, line %d\n", line);
      continue;
    }
    if (strcmp(user, u) == 0) {
      memcpy(info->user, user, strlen(user));
      char* passwd = strtok(NULL, ",");
      if (!passwd) {
        fprintf(stderr, "illegal form in account file, line %d\n", line);
        continue;
      }
      md5(passwd, strlen(passwd), info->passwd_hash);
      ret = 1;
      break;
    }
  }
  free(buf);
  fclose(file);
  return ret;
}

//handle the login request from client
static void login(
  csiebox_server* server, int conn_fd, csiebox_protocol_login* login) {
  int succ = 1;
  csiebox_client_info* info =
    (csiebox_client_info*)malloc(sizeof(csiebox_client_info));
  memset(info, 0, sizeof(csiebox_client_info));
  if (!get_account_info(server, login->message.body.user, &(info->account))) {
    fprintf(stderr, "cannot find account\n");
    succ = 0;
  }
  if (succ &&
      memcmp(login->message.body.passwd_hash,
             info->account.passwd_hash,
             MD5_DIGEST_LENGTH) != 0) {
    fprintf(stderr, "passwd miss match\n");
    succ = 0;
  }

  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  header.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
  header.res.op = CSIEBOX_PROTOCOL_OP_LOGIN;
  header.res.datalen = 0;
  if (succ) {
    if (server->client[conn_fd]) {
      free(server->client[conn_fd]);
    }
    info->conn_fd = conn_fd;
    server->client[conn_fd] = info;
    header.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
    header.res.client_id = info->conn_fd;
    char* homedir = get_user_homedir(server, info);
    mkdir(homedir, DIR_S_FLAG);
    free(homedir);
  } else {
    header.res.status = CSIEBOX_PROTOCOL_STATUS_FAIL;
    free(info);
  }
  send_message(conn_fd, &header, sizeof(header));
}

static void logout(csiebox_server* server, int conn_fd) {
  free(server->client[conn_fd]);
  server->client[conn_fd] = 0;
  close(conn_fd);
}

static char* get_user_homedir(
  csiebox_server* server, csiebox_client_info* info) {
  char* ret = (char*)malloc(sizeof(char) * PATH_MAX);
  memset(ret, 0, PATH_MAX);
  sprintf(ret, "%s/%s", server->arg.path, info->account.user);
  return ret;
}

static int handle_download(csiebox_server *server,int conn_fd)
{
	FTS *fts;
	FTSENT *ftsent_head;
	FTSENT *p1,*p2;
	struct dirent *dir;
	DIR *Dir;
	char path[PATH_MAX];
	char *user_dir = get_user_homedir( server, server->client[conn_fd]);
	char *a[] = {user_dir,NULL};
	int wd;
	int numFD = 0;
	csiebox_protocol_status status;

	if( !(Dir = opendir(user_dir)))
	{
		fprintf(stderr,"opendir fail: %s\n",user_dir);
		return -1;
	}
	while( dir = readdir(Dir) )
	{
		fprintf(stderr,"%s\n",dir->d_name);
		numFD++;
	}
	closedir(Dir);
	fprintf(stderr,"Num of file and dir: %d\n",numFD);

	if( numFD < 3 )
	{
		// if cdir is empty, download from server
		fprintf(stderr,"empty dir, end download\n");
		return -1;
	}

	fts = fts_open( a, FTS_PHYSICAL |  FTS_NOCHDIR, NULL);
	if( fts == NULL )
	{
		fprintf(stderr, "fts fail\n");
		return -1;
	}
	
	while( ( ftsent_head = fts_read(fts)) != NULL )
	{
		ftsent_head = fts_children( fts,0);

		for( p1 = ftsent_head; p1 != NULL; p1 = p1->fts_link )
		{		
			memset(path,0,PATH_MAX);
			sprintf(path,"%s/%s",p1->fts_path,p1->fts_name);
			// if the file is hidden, ignore it
			if( p1->fts_name[0] == '.' )
			{
				continue;
			}
			if( strcmp(path, user_dir) ==0 )
			{
				continue;
			}
			switch( p1->fts_info )
			{
				case FTS_D:
					fprintf(stderr,"start sync dir %s\n",path);
					status = server_send_meta( path, conn_fd, server);
					if( status != CSIEBOX_PROTOCOL_STATUS_OK )
					{
						fprintf( stderr, "receive status fail\n");
					}
					break;
				case FTS_F:
					if( p1->fts_statp->st_nlink == 1)
					{	
						// there is no hard link
						fprintf(stderr, "start sync file %s\n",path);

						status = server_send_meta( path, conn_fd, server);
						if( status == CSIEBOX_PROTOCOL_STATUS_OK )
						{
							fprintf(stderr,"receive status ok\n");
						}	
						else if( status == CSIEBOX_PROTOCOL_STATUS_MORE )
						{
							server_send_file(path, conn_fd, server);
						}
						else
						{
							fprintf(stderr,"receive status fail\n");
						}
					}
					else
					{
						// there might be hard link
						p2 = ftsent_head;
						char path2[PATH_MAX];
						while(p2 != p1 )
						{
							memset(path2,0,PATH_MAX);
							sprintf(path2,"%s/%s",p2->fts_path,p2->fts_name);
							if( p2->fts_statp->st_ino == p1->fts_statp->st_ino )
							{
								// if it is a hardlink
								fprintf(stderr, "start sync hardlink %s\n",path);
								server_send_hardlink(path, path2, conn_fd, server);
								break;
							}
							p2 = p2->fts_link;
						}
						if( p2 == p1 )
						{
							fprintf(stderr, "start sync file %s\n",path);

							status = server_send_meta( path, conn_fd, server);
							if( status == CSIEBOX_PROTOCOL_STATUS_OK )
							{
								fprintf(stderr,"receive status ok\n");
							}	
							else if( status == CSIEBOX_PROTOCOL_STATUS_MORE )
							{
								server_send_file(path, conn_fd, server);
							}
							else
							{
								fprintf(stderr,"receive status fail\n");
							}
						}
					}				
					break;
				case FTS_SL:
					fprintf(stderr, "start sync symbolic %s\n",path);
					status = server_send_meta( path, conn_fd, server);
					if( status == CSIEBOX_PROTOCOL_STATUS_OK )
					{
						fprintf(stderr,"receive status ok\n");
					}	
					else if( status == CSIEBOX_PROTOCOL_STATUS_MORE )
					{
						server_send_symblink(path, conn_fd, server);
					}
					else
					{
						fprintf(stderr,"receive status fail\n");
					}
					break;
				case FTS_SLNONE:
					fprintf(stderr, "start sync symbolic %s\n",path);
					status = server_send_meta( path, conn_fd, server);
					if( status == CSIEBOX_PROTOCOL_STATUS_OK )
					{
						fprintf(stderr,"receive status ok\n");
					}	
					else if( status == CSIEBOX_PROTOCOL_STATUS_MORE )
					{
						server_send_symblink(path, conn_fd, server);
					}
					else
					{
						fprintf(stderr,"receive status fail\n");
					}
					break;
				default:
					fprintf(stderr,"Unknown type of fts_info\n");
					break;
			}
		}
	}
}

void* handle_req_loop(void* data)
{
	int thr_num = *((int*)data);
	pthread_mutex_lock(&req_mutex);
	fprintf(stderr, "Thread%d starts\n", thr_num);
	while(1){
		if ( req_num > 0)
		{
			fprintf(stderr, "BEFORE: the req num = %d\n", req_num);
			struct request *a_request;
			thr_state[thr_num] = 0; 											//set the thread state busy
			signal_unblockUSR1();													//unblock the sigusr1
			a_request = get_req(req_queue, last_req, &req_num);
			int rc = pthread_mutex_unlock(&req_mutex);

			if ( a_request)
			{
				fprintf(stderr, "thread%d start to handle request, conn_fd is %d\n", thr_num, a_request->conn_fd);
				handle_request(a_request->server, a_request->conn_fd);
				client_state[a_request->conn_fd] = 0;
				free(a_request);
				signal_blockUSR1();													//block the sigusr1
				thr_state[thr_num] = 1; 										//set the thread state idle
				fprintf(stderr, "thread%d finish handling request, conn_fd is %d\n", thr_num, a_request->conn_fd);
			  fprintf(stderr, "AFTER: the req num = %d\n", req_num);
			}
		}
		else
		{
			fprintf(stderr, "thread%d waits for the request\n", thr_num);
			fflush(stderr);
			pthread_cond_wait(&got_req, &req_mutex);
			fprintf(stderr, "thread%d gets the request\n", thr_num);
			fflush(stderr);
		}
	}
}

void add_req( pthread_mutex_t *mutex, pthread_cond_t *cond_t, const int *dim, struct request *request)
{
	pthread_mutex_lock(mutex); 										//lock the mutex
	client_state[request->conn_fd] = 1;
	if(check_thr(dim) == 0){						//cheak whether there is free thread		
		if(req_num == 0){ 													//the queue is empty
			req_queue = request;
			last_req = request;
		}
		else{
			last_req->next = request;
			last_req = request;
		}
		req_num++;
		fprintf(stderr, "Add a request(conn_fd: %d)\n", last_req->conn_fd);
		fflush(stderr);
		pthread_mutex_unlock(mutex); // unlock the mutex
		pthread_cond_signal(cond_t); // send signal 
	}
	else{
		fprintf(stderr, "thread all busy!\n");
		server_busy_res(request->conn_fd);
		client_state[request->conn_fd] = 0;
		free(request);
		pthread_mutex_unlock(mutex);
	}

	fprintf(stderr, "req_num: %d\n", req_num);


}

int check_thr(const int *dim)
{
	for ( int i = 0; i < *dim; ++i) if ( thr_state[i] == 1)
	{ 
		return 0;
	}
	return -1;
}

struct request* set_request( int conn_fd, csiebox_server *server)
{
	struct request *tmp = (struct request*)malloc(sizeof(struct request));
	tmp->server = server;
	tmp->conn_fd = conn_fd;
	tmp->next = NULL;
	return tmp;
}

struct request* get_req()
{

	struct request *req;
	if ( req_num == 0) req = NULL;
	else
	{
		req = req_queue;
		req_queue = req_queue->next;
		if ( req_queue == NULL) last_req = NULL;
		req_num--;
	}
	return req;
}

static void signal_blockUSR1()
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);

}
static void signal_unblockUSR1()
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}


void signal_handler(int snum)
{
	fprintf(stderr, "receive SIGUSR1\n");
	int fd = open(FIFO_FILE, O_WRONLY);
	for (int i = 0; i < thr_state_size; i++)
	{
		if (thr_state[i] == 0)
		{
			uint32_t working_thread = i;
			working_thread = htonl(working_thread);
			write(fd, &working_thread, sizeof(uint32_t));
		}
	}
	close(fd);
}

void signal_handler_del(int snum)
{
	remove(FIFO_FILE);
	//remove(pid_file);
	exit(0);
}


static void daemonize (char* run_path)
{
	pid_t process_id = 0;
	pid_t sid = 0;
	snprintf(pid_file, PATH_MAX, "%s/csiebox_server.pid", run_path);
	
	process_id = fork();

	if (process_id < 0)
	{
		fprintf(stderr, "fork failed!\n");
		exit(1);
	}
	if (process_id > 0)
	{
		fprintf(stderr, "Terminate the parent, and start to daemonize(child pid = %d)\n", process_id);
		exit(0);
	}
	
	pid_t chld_pid = getpid();
	FILE* fd_pid = fopen(pid_file, "w+");
	fprintf(fd_pid, "%d", chld_pid);
	fclose(fd_pid);

	umask(0);
	sid = setsid();
	if (sid < 0) exit(1);
	
	// attach stdin out err to  /dev/null;
	int fd0 = open ("/dev/null", O_RDWR);
	close(0);
	close(1);
	close(2);
	dup2(fd0, 0);
	dup2(fd0, 1);
	dup2(fd0, 2);
	close(fd0);
}
