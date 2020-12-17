#include "http_conn.h"
/*定义HTTP响应的一些状态信息*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
/*网站的根目录*/
const char* doc_root = "/var/www/html";

int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

void addfd( int epollfd, int fd, bool one_shot )
{
    epoll_event event;
    event.data.fd = fd;
    /*EPOLLRDHUP：TCP连接被对方关闭，或者对方关闭了写操作*/
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot )
    {
        /*在一个连接中，防止一个线程从一个sockfd中获取了数据，并且正在处理该数据时，又有其他数据过来，这是分配另外一个线程获取剩余数据。
        设置EPOLLONESHOT后，其他线程不能触发epollin事件，换句话说，如果一个线程处理完一个sockfd后，需要充值socket上的epolloneshot事件，确保其他线程可以处理这个线程*/
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    /*ET：应用必须立即处理事件，后续epoll_wait()不会再次通知此次事件，LT模式将持续通知直到事件被处理。
    例如，如果epoll_wait()触发EPOLLIN事件，要从sockfd读入数据，由于TCP流，一次不一定能读完，所以在LT中，可以多次读，即每次循环时，该sockfd的EPOLLIN都处于触发状态，知道sockfd缓冲区没数据为止：
    if(events[i].events&EPOLLIN)
    {
        int ret = recv(sockfd,buf,buffer_size-1,0);
        if(ret<=0)
        {
            close(sockfd);
            continue;
        }
    }
    而ET模式则需要一次读完：
    if(events[i].events&EPOLLIN)
    {
        while(1)
        {
            int ret = recv(sockfd,buf,buffer_size-1,0);
            if(ret<=0)
            {
                if((errno==EAGAIN)||(errno==EWOULDBLOCK))
                {
                    break;
                }
                close(sockfd);
                break;
            }
        }
    }
    ET模式应该设置为非阻塞式,否则读操作会因为没有后续的数据进入sockfd缓冲区而一直阻塞下去*/
    setnonblocking( fd );
}

void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}
/*重置fd*/
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn( bool real_close )
{
    if( real_close && ( m_sockfd != -1 ) )
    {
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--;/*关闭一个连接时，将客户总量减1*/
    }
}

void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;
    /*
    int error = 0;
    socklen_t len = sizeof( error );
    getsockopt( m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len );
    */
    /*下两行是为了避免time_wait状态，仅用于调试，实际使用时应该去掉*/
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}
/*从状态机，用于解析一行内容*/
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    /*m_checked_idx指向m_read_buf应用进程的读缓冲区中当前正在分析的字节，m_read_idx指向缓冲中客户数据的尾部的下一字节，0~m_checked_idx都已经分析完毕，第m_checked_idx~m_read_idx-1字节由下面循环分析*/
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
        /*当前要分析的字节*/
        temp = m_read_buf[ m_checked_idx ];
        /*如果当前的字节是\r，即回车符，说明可能读到一个完整的行*/
        if ( temp == '\r' )
        {
            /*如果\r字符碰巧是目前缓冲区的最后一个已经被读入的客户数据，则这一次分析没有读到一个完整的行，返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析*/
            if ( ( m_checked_idx + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
            /*如果下一个数据是\n,则表明我们成功读到一个完整的行*/
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' )
            {
                /*把\r\n替换成\0*/
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            /*否则，说明客户发送的HTTP请求存在语法问题*/
            return LINE_BAD;
        }
        /*如果当前的字节是\n，即换行符，则也说明可能读到一个完整的行*/
        else if( temp == '\n' )
        {
            if( ( m_checked_idx > 1 ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) )
            {
                /*把\r\n替换成\0*/
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    /*如果所有内容都被分析完毕，都没有遇到\r，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析*/
    return LINE_OPEN;
}
/*循环读取数据，直到无数据可读或者对方关闭连接*/
bool http_conn::read()
{
    if( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }

    int bytes_read = 0;
    while( true )
    {
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if ( bytes_read == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 )
        {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}
/*解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
GET /chapter15/index.html HTTP/1.1
*/
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
    /*在text中，一次校验后面字符串中的每个字符，返回text中第一个匹配的字符的指针，若没有匹配，则返回NULL*/
    m_url = strpbrk( text, " \t" );
    /*如果请求行中没有空白字符或\t字符，则HTTP请求必有问题*/
    if ( ! m_url )
    {
        return BAD_REQUEST;
    }
    /**/
    *m_url++ = '\0';

    char* method = text;
    /*仅支持GET方法*/
    if ( strcasecmp( method, "GET" ) == 0 )
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    /*返回第一个不在m_url中匹配的下标，开始时m_url指向目标url+http版本号，如今指向版本号，是这样的吗？*/
    m_url += strspn( m_url, " \t" );
    m_version = strpbrk( m_url, " \t" );
    if ( ! m_version )
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn( m_version, " \t" );
    /*仅支持HTTP/1.1*/
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 )
    {
        return BAD_REQUEST;
    }
    /*检测url是否合法*/
    if ( strncasecmp( m_url, "http://", 7 ) == 0 )
    {
        m_url += 7;
        m_url = strchr( m_url, '/' );
    }

    if ( ! m_url || m_url[ 0 ] != '/' )
    {
        return BAD_REQUEST;
    }
    /*HTTP请求行处理完毕，状态转移到头部字段的分析*/
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
/*解析HTTP请求的一个头部信息*/
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
    /*遇到空行，表示头部子段解析完毕*/
    if( text[ 0 ] == '\0' )
    {
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }

        /*如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态*/
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        /*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }
    /*处理connection头部子段*/
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            /*用来设定“SOCKET在CLOSE时候是否等待缓冲区发送完成*/
            m_linger = true;
        }
    }
    /*处理content-length头部子段*/
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
    /*处理host头部字段*/
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;

}
/*并没有真正的解析HTTP请求的消息体，只是判断它是否被完整的读入了*/
http_conn::HTTP_CODE http_conn::parse_content( char* text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}
/*主状态机*/
http_conn::HTTP_CODE http_conn::process_read()
{
    /*记录当前行的读取状态*/
    LINE_STATUS line_status = LINE_OK;
    /*记录HTTP请求的处理结果*/
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ( ( ( m_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
                || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
        /*char* get_line() { return m_read_buf + m_start_line; } 读缓冲+解析行的起始位置*/
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        /*著状态机当前所处状态*/
        switch ( m_check_state )
        {
            /*分析请求行*/
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                break;
            }
            /*分析请求头*/
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content( text );
                if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}
/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    if ( stat( m_real_file, &m_file_stat ) < 0 )
    {
        return NO_RESOURCE;
    }

    if ( ! ( m_file_stat.st_mode & S_IROTH ) )
    {
        return FORBIDDEN_REQUEST;
    }

    if ( S_ISDIR( m_file_stat.st_mode ) )
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}
/*对内存映射区执行munmap操作*/
void http_conn::unmap()
{
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}
/*写HTTP响应*/
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    /*如果*/
    
    if ( bytes_to_send == 0 )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while( 1 )
    {
        temp = writev( m_sockfd, m_iv, m_iv_count );
        
        if ( temp <= -1 )
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但这可以保证连接的完整性*/
            if( errno == EAGAIN )
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send )
        {
            /*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            unmap();
            if( m_linger )
            {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            }
            else
            {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}
/*往写缓冲中写入待发送的数据*/
bool http_conn::add_response( const char* format, ... )
{
    /*写缓冲区已满*/
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length( int content_len )
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}
/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::process_write( HTTP_CODE ret )
{
    switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            /*bool http_conn::add_status_line( int status, const char* title )
            {
                return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
            }*/
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title );
            if ( m_file_stat.st_size != 0 )
            {
                add_headers( m_file_stat.st_size );
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if ( ! add_content( ok_string ) )
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
/*由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

