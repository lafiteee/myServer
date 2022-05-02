#include "http_conn.h"

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    // if (one_shot) {
    //     event.events |= EPOLLONESHOT;
    // }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
void remove(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件可以触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

http_conn::http_conn() {

}

http_conn::~http_conn() {

}

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;

    init();

}

void http_conn::init() {
    // 初始化状态为解析请求首行
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_checked_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_url = 0;
    m_version = 0;
    m_method = GET;
    m_linger = 0;
    m_host = 0;

    bzero(m_read_buf, READ_BUF_SIZE);
}

// 关闭连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {
        remove(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 非阻塞读，循环读取客户端数据，直到无数据可读
bool http_conn::read() {
    if (m_read_idx >= READ_BUF_SIZE) {
        return false;
    }
    // 已经读取到的字节
    int bytes_read = 0;
    while (1) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUF_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据了
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        // printf("bytes_read: %d\n", bytes_read);
        m_read_idx += bytes_read;
    }
    printf("读取到了数据：%s\n", m_read_buf);
    return true;
}

// 解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;
    while ( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || (line_status = parse_line()) == LINE_OK) {
        // 解析到了一行完整的数据，或者解析到了请求体，也是完整的数据

        // 获取一行数据
        text = get_line();
        // 更新m_start_line
        m_start_line = m_checked_index;
        printf("got 1 http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                // 后续还要对不同的情况做不同的判断
                break;
            }

            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {    // 解析到了完整的请求头
                    return do_request();    
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
        return NO_REQUEST;
    }
}
// 解析HTTP请求首行，获得请求方法、目标URL、HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    m_url = strpbrk(text, " \t");
    *m_url++ = '\0';
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 修改主状态机的检查状态变成检查请求头
    return NO_REQUEST;

}
// 解析HTTP请求头
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    if (text[0] == '\n') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}
// 解析HTTP请求体
http_conn::HTTP_CODE http_conn::parse_content(char* text){

}
// 解析行，判断依据 \r\n
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;

    for (; m_checked_index < m_read_idx; m_checked_index++) {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r') {
            if (m_checked_index + 1 == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_index + 1] == '\n') {
                // 把 \r\n 变成 \0\0，标记读取行数据字符串的终止
                m_read_buf[m_checked_index++] = '\0'; 
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            } else {
                return LINE_BAD;
            }
        } else if (temp == '\n') {
            if (m_checked_index > 1 && m_read_buf[m_checked_index - 1] == '\r') {
                m_read_buf[m_checked_index-1] = '\0'; 
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            } else {
                return LINE_BAD;
            }
        }
        return LINE_OPEN;
    }
}

http_conn::HTTP_CODE do_request() {

}




// 非阻塞写
bool http_conn::write() {
    std::cout << "一次性写完数据" << std::endl;
    return true;
}

// 由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process() {

    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    // 生成响应
}
