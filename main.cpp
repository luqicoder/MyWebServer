#include "webserver.h"

int main(int argc, char *argv[])
{
    WebServer server;

    //初始化
	server.init(12345, 8);
    

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //监听
    server.eventListen();

    //运行
    server.eventLoop_src();

    return 0;
}
