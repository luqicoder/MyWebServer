#include "webserver.h"
#include "log.h"

WebServer::WebServer()
{
    //http_conn类对象,预先为每个可能的客户连接分配一个http_conn对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[11] = "/root/html";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);
	printf("root:%s\n", m_root);
    //定时器
    users_timer = new client_data[MAX_FD];
}


WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port ,int thread_num)
{	
	 //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "147258369";
    string databasename = "http_serv";

	int log_write = 1;//日志写入方式
    int opt_linger = 0;//优雅关闭
	int trigmode = 3;//ET+ET
	int sql_num = 8;//数据库连接数量
	int close_log = 0;//是否关闭日志，0表示开启
	int actor_model = 0;//proactor模式

	init(port, user, passwd, databasename, log_write, opt_linger, trigmode, sql_num, thread_num, close_log, actor_model);
	trig_mode();

}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}


void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

// void WebServer::sql_pool()
// {
//     //初始化数据库连接池
//     m_connPool = connection_pool::GetInstance();
//     m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

//     //初始化数据库读取表
//     users->initmysql_result(m_connPool);
// }

void WebServer::thread_pool()
{
    //线程池
    //m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
	m_pool = new threadpool<http_conn>(m_thread_num);
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);
	//初始化客户连接
	//users[connfd].init(connfd, client_address);
	users[connfd].init(connfd, client_address, m_root, m_close_log, m_user, m_passWord, m_databaseName);
    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

void WebServer::adjust_timer(util_timer *timer)
{
	time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
	//printf("adjust timer once\n");
    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
	//等价于users[sockfd].close_conn();
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
	socklen_t client_addrlength = sizeof(client_address);
	//由于监听套接字设置为ET模式，当多个连接同时到达监听端口，监听队列由空变为非空，EPOLLIN事件被触发，如果不加while则只进行一次accept，也只有一个连接被接受。
	//之后尽管监听队列非空，在ET模式下，也不会再次触发EPOLL事件了。
	while (1) 
	{
		int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
		if (connfd < 0)
		{
			//printf("dealclinetdata errno is: %d\n", errno);
			LOG_ERROR("%s:errno is:%d", "accept error", errno);
			break;
		}
		if (http_conn::m_user_count >= MAX_FD)
		{
			//utils.show_error(connfd, "Internal server busy");
			LOG_ERROR("%s", "Internal server busy");
			break;
		}
		timer(connfd, client_address);
	}  
    return false;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
				//用timeout变量标记有定时任务处理，但不立即处理定时任务。因为定时任务优先级并不高，需要优先处理其他更重要的任务
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
	util_timer *timer = users_timer[sockfd].timer;
	if (users[sockfd].read())
	{
		LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

		//若监测到读事件，将该事件放入请求队列
		//m_pool->append_p(users + sockfd);
		m_pool->append(users + sockfd);

		if (timer)
		{
			adjust_timer(timer);
		}
	}
	else
	{
		deal_timer(timer, sockfd);
	}
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

	//proactor
	if (users[sockfd].write())
	{
		LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
		//如果某个客户连接上有数据可读，则需要调整该连接对应定时器，以延迟该连接被关闭的时间
		if (timer)
		{
			adjust_timer(timer);
		}
	}
	else
	{
		deal_timer(timer, sockfd);
	}
   
}


void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
	//SO_LINGER选项用于控制close系统调用在关闭TCP连接时的行为(p93)
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

	//设置信号处理函数
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);//定时

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::eventLoop_src()
{
	bool timeout = false;
    bool stop_server = false;

	while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
			LOG_ERROR("%s", "epoll failure");
            //printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
			//处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //如果有异常，直接关闭客户连接
                //users[sockfd].close_conn();
				//服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
			//处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
					
            }
            else if(events[i].events & EPOLLIN)
            {
                //根据读的结果，决定是将任务添加到线程池，还是关闭连接
                dealwithread(sockfd);
            }
            else if(events[i].events & EPOLLOUT)
            {
                //根据写的结果，决定是否关闭连接
                dealwithwrite(sockfd);
            }
        }

		if (timeout)
		{
			utils.timer_handler();

			LOG_INFO("%s", "timer tick");

			timeout  = false;
		}
        
        
    }
}

