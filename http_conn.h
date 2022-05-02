#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <iostream>
#include <sys/epoll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>

#include "locker.h"

class http_conn {
public:

    // 所有的socket上的事件都被注册到一个epoll上
    static int m_epollfd;
    // 统计用户连接数量
    static int m_user_count;
    static const int READ_BUF_SIZE = 2048;      // 读缓冲区大小
    static const int WRITE_BUF_SIZE = 1024;     // 写缓冲区大小
    
    // HTTP请求方法
    enum METHOD {GET=0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    // 解析客户端请求时，主状态机的状态
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE=0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    // 从状态机的三种可能状态
    enum LINE_STATUS {LINE_OK=0, LINE_BAD, LINE_OPEN};
    // 服务器处理HTTP请求的可能结果，报文解析的结果
    enum HTTP_CODE {NO_REQUEST=0, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};



    http_conn();

    ~http_conn();

    // 初始化新接收的连接
    void init(int sockfd, const sockaddr_in& addr);

    // 关闭连接
    void close_conn();

    // 非阻塞读
    bool read();

    // 非阻塞写
    bool write();

    void process();
    // 解析HTTP请求
    HTTP_CODE process_read();
    // 解析HTTP请求首行
    HTTP_CODE parse_request_line(char* text);
    // 解析HTTP请求头
    HTTP_CODE parse_headers(char* text);
    // 解析HTTP请求体
    HTTP_CODE parse_content(char* text);
    // 解析行
    LINE_STATUS parse_line();

private:

    int m_sockfd;   // 该HTTP连接的socket
    sockaddr_in m_address;  // 通信的socket地址
    char m_read_buf[READ_BUF_SIZE]; // 读缓冲区
    int m_read_idx;     // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    // 当前正在分析的字符在读缓冲区的位置
    int m_checked_index;
    // 当前正在解析的行的起始位置
    int m_start_line;
    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;

    char* m_url;
    char* m_version;
    METHOD m_method;
    char* m_host;   // 主机名
    bool m_linger;  // HTTP是否保持连接
    int m_content_length;

    // 初始化
    void init();

    inline char* get_line() {return m_read_buf + m_start_line;}

    http_conn::HTTP_CODE do_request();

};


#endif