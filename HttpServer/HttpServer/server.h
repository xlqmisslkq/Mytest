#pragma once
//服务器端要处理的业务逻辑
// 初始化监听的文件描述符
int initListenFD(unsigned short port);
//启动epoll
int epollRun(unsigned short port);
//和客户端建立连接
int acceptconn(int lfd, int epfd);
//接收客户端的http请求消息
int revHttpRequest(int cfd,int epfd);
//解析请求行
int parseRequestLine(int cfd,const char* reqLine);
//发送http响应的状态行+响应头+空行
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length);
//发送目录
int sendDir(int cfd, const char* filename);
//发送文件
int sendFile(int cfd, const char* filename);
const char* getFileType(const char* name);
int hexit(char c);
void decodeMsg(char* to, char* from);
// 和客户端断开连接
int disConnect(int cfd, int epfd);