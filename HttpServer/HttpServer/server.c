#include "server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

int initListenFd(unsigned short port)
{
	// 1. 创建监听的套接字
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1)
	{
		perror("socket");
		return -1;
	}

	// 2. 设置端口复用
	int opt = 1;
	int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (ret == -1)
	{
		perror("setsockopt");
		return -1;
	}

	// 3. 绑定
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);	// 
	addr.sin_addr.s_addr = INADDR_ANY;
	ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1)
	{
		perror("bind");
		return -1;
	}
	// 4. 设置监听
	ret = listen(lfd, 128);
	if (ret == -1)
	{
		perror("listen");
		return -1;
	}
	// 5. 将得到的可用的套接字返回个调用者
	return lfd;
}

int epollRun(unsigned short port)
{
	// 1. 创建epoll模型
	int epfd = epoll_create(10);
	if (epfd == -1)
	{
		perror("epoll_create");
		return -1;
	}

	// 2. 初始化epoll模型
	int lfd = initListenFd(port);
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = lfd;
	// 添加 lfd 到检测模型中
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1)
	{
		perror("epoll_ctl");
		return -1;
	}
	// 检测 - 循环检测
	struct epoll_event evs[1024];
	int size = sizeof(evs) / sizeof(evs[0]);
	int flag = 0;
	while (1)
	{
		if (flag)
		{
			break;
		}
		// 主线程不停的调用epoll_wait
		int num = epoll_wait(epfd, evs, size, -1);
		for (int i = 0; i < num; ++i)
		{
			int curfd = evs[i].data.fd;
			if (curfd == lfd)
			{
				// 建立新连接
				// 创建子线程, 在子线程中建立新的连接
				// acceptConn是子线程的回调
				int ret = acceptConn(lfd, epfd);
				if (ret == -1)
				{
					// 规定: 建立连接失败, 直接终止程序
					int flag = 1;
					break;
				}
			}
			else
			{
				// 通信 -> 先接收数据, 然后回复数据
				// 创建子线程, 在子线程中通信, recvHttpRequest是子线程的回调
				recvHttpRequest(curfd, epfd);
			}
		}
	}
	return 0;
}

int acceptConn(int lfd, int epfd)
{
	// 1. 建立新连接
	int cfd = accept(lfd, NULL, NULL);
	if (cfd == -1)
	{
		perror("accept");
		return -1;
	}

	// 2. 设置通信文件描述符为非阻塞
	int flag = fcntl(cfd, F_GETFL);
	flag |= O_NONBLOCK;
	fcntl(cfd, F_SETFL, flag);

	// 3. 通信的文件描述符添加到epoll模型中
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET; // 边沿模式
	ev.data.fd = cfd;
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1)
	{
		perror("epoll_ctl");
		return -1;
	}

	return 0;
}

int recvHttpRequest(int cfd, int epfd)
{
	// 因为是边沿非阻塞模式, 所以循环读数据
	char tmp[1024];	// 每次接收1k数据
	char buf[4096];	// 将每次读到的数据存储到这个buf中
	// 循环读数据
	int len, total = 0; // total: 当前buf中已经存储了多少数据
	// 没有必要将所有的http请求全部保存下来
	// 因为需要的数据都在请求行中
	// - 客户端向服务器请求都是静态资源, 请求的资源内容在请求行的第二部分
	// - 只需要将请求完整的保存下来就可以, 请求行后边请求头和空行
	// - 不需要解析请求头中的数据, 因此接收到 之后不存储也是没问题的
	while ((len = recv(cfd, tmp, sizeof(tmp), 0)) > 0)
	{
		if (total + len < sizeof(buf))
		{
			// 有空间存储数据
			memcpy(buf + total, tmp, len);
		}
		total += len;
	}

	// 循环结束 -> 读完了
	// 读操作是非阻塞的, 当前缓存中没有数据值返回-1, errno==EAGAIN
	if (len == -1 && errno == EAGAIN)
	{
		// 将请求行从接收的数据中拿出来
		// 在http协议中换行使用的是 \r\n
		// 遍历字符串, 当遇到第一个\r\n的时候意味着请求行就拿到了
		// buf中存储了接收的客户端的http请求数据
		char* pt = strstr(buf, "\r\n");
		// 请求行字节数(长度)
		int reqlen = pt - buf;
		// 保留请求行就可以
		buf[reqlen] = '\0';	// 字符串截断
		// 解析请求行
		parseRequestLine(cfd, buf);
	}
	else if (len == 0)
	{
		printf("客户端断开了连接...\n");
		// 服务器和客户端断开连接, 文件描述符从epoll模型中删除
		disConnect(cfd, epfd);
	}
	else
	{
		perror("recv");
		return -1;
	}
	return 0;
}

int parseRequestLine(int cfd, const char* reqLine)
{
	// 请求行分为三部分
	// GET /helo/world/ http/1.1
	// 1. 将请求行的三部分依次拆分, 有用的前两部分
	//	- 提交数据的方式
	//  - 客户端向服务器请求的文件名
	char method[6];
	// 存储了请求行中的第二部分数据
	// 客户端向服务器请求的静态文件的名字/目录名
	// 如果是静态请求的话, path中不会携带客户端向服务器提交的动态数据
	char path[1024];
	sscanf(reqLine, "%[^ ] %[^ ]", method, path);

	// 2. 判断请求方式是不是get, 不是get直接忽略
	// http中不区分大小写 get / GET / Get
	if (strcasecmp(method, "get") != 0)
	{
		printf("用户提交的不是get请求, 忽略...\n");
		return -1;
	}

	// 3. 判断用户提交的请求是要访问服务器端的文件还是目录
	//	 /helo/world/
	//	- 第一个 / : 服务器的提供资源根目录, 在服务器端可以随意指定
	//	- hello/world/ -> 服务器资源根目录中的两个目录
	// 需要在程序中判断得到的文件的属性 - stat()
	// 判断path中存储的到底是什么字符串?
	char* file = NULL; // file中保存的文件路径是相对路径
	// 如果文件名中有中文, 需要还原
	decodeMsg(path, path);
	if (strcmp(path, "/") == 0)
	{
		// 访问的是服务器提供的资源根目录 假设是: /home/robin/luffy
		// / 不是系统根目录, 是服务器提供个资源目录 == 传递进行的 /home/robin/luffy
		// 如何在服务器端将服务器的资源根目录描述出来?
		// - 在启动服务器程序的时候, 先指定资源根目录是那个目录
		// - 在main函数中将工作目录切换到了资源根目录  /home/robin/luffy
		// - 在这里 ./ ==  /home/robin/luffy
		file = "./";	// ./ 对应的目录就是客户端访问的资源的根目录
	}
	else
	{
		// 假设是这样的: /hello/a.txt
		//	/ ==  /home/robin/luffy ==> /home/robin/luffy/hello/a.txt
		// 如果不把 / 去掉就相当于要访问系统的根目录
		file = path + 1; // hello/a.txt == ./hello/a.txt
	}

	printf("客户端请求的文件名: %s\n", file);

	// 属性判断
	struct stat st;
	// 第一个参数是文件的路径, 相对/绝对, file中存储的是相对路径
	int ret = stat(file, &st);
	if (ret == -1)
	{
		// 获取文件属性失败 ==> 没有这个文件
		// 给客户端发送404页面
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
		sendFile(cfd, "404.html");
	}
	if (S_ISDIR(st.st_mode))
	{
		// 遍历目录, 将目录的内容发送给客户端
		// 4.  客户端请求的名字是一个目录, 遍历目录, 发送目录内容给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
		//sendDir(cfd, file); // <table></table>
	}
	else
	{
		// 如果是文件, 发送文件内容给客户端
		// 5. 客户端请求的名字是一个文件, 发送文件内容给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
		sendFile(cfd, file);
	}

	return 0;
}


/*
	status: 状态码
	descr: 状态描述
	type: Content-Type 的值, 要回复的数据的格式
	length: Content-Length 的值, 要回复的数据的长度
*/
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length)
{
	// 状态行 + 消息报头 + 空行
	char buf[4096];
	// http/1.1 200 ok
	sprintf(buf, "http/1.1 %d %s\r\n", status, descr);
	// 消息报头 -> 2个键值对
	// content-type:xxx	== > https://tool.oschina.net/commons
	//	.mp3 ==>  audio/mp3
	sprintf(buf + strlen(buf), "Content-Type: %s\r\n", type);
	// content-length:111
	// 空行
	sprintf(buf + strlen(buf), "Content-Length: %d\r\n\r\n", length);
	// 拼接完成之后, 发送
	send(cfd, buf, strlen(buf), 0);
	return 0;
}

int sendFile(int cfd, const char* fileName)
{
	// 在发送内容之前应该有 状态行+消息报头+空行+文件内容
	// 这四部分数据需要组织好之后再发送吗?
	//	- 不需要, 为什么? -> 传输层是默认使用的tcp
	//	面向连接的流式传输协议 -> 只有最后全部发送完就可以
	//读文件内容, 发送给客户端
	// 打开文件
	int fd = open(fileName, O_RDONLY);
	// 循环读文件
	while (1)
	{
		char buf[1024] = { 0 };
		int len = read(fd, buf, sizeof(buf));
		if (len > 0)
		{
			// 发送读出的文件内容
			send(cfd, buf, len, 0);
			// 发送端发送太快会导致接收端的显示有异常,接收端解析不过来造成数据丢失
			usleep(50);
		}
		else if (len == 0)
		{
			// 文件读完了
			break;
		}
		else
		{
			perror("读文件失败...\n");
			return -1;
		}
	}
	return 0;
}

/*
	客户端访问目录, 服务器需要遍历当前目录, 并且将目录中的所有文件名发送给客户端即可
	- 遍历目录得到的文件名需要放到html的表格中
	- 回复的数据是html格式的数据块
	<html>
		<head>
			<title>test</title>
		</head>
		<body>
			<table>
				<tr>
					<td>文件名</td>
					<td>文件大小</td>
				</tr>
			</table>
		</body>
	<html>
*/
// opendir readdir closedir
int sendDir(int cfd, const char* dirName)
{
	char buf[4096];
	struct dirent** namelist;
	sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
	int num = scandir(dirName, &namelist, NULL, alphasort);
	for (int i = 0; i < num; ++i)
	{
		// 取出文件名
		char* name = namelist[i]->d_name;
		// 拼接当前文件在资源文件中的相对路径
		char subpath[1024];
		sprintf(subpath, "%s/%s", dirName, name);
		struct stat st;
		// stat函数的第一个参数文件的路径
		stat(subpath, &st);
		if (S_ISDIR(st.st_mode))
		{
			// 如果是目录, 超链接的跳转路径文件名后边加 /
			sprintf(buf + strlen(buf),
				"<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>",
				name, name, (long)st.st_size);
		}
		else
		{
			sprintf(buf + strlen(buf),
				"<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
				name, name, (long)st.st_size);
		}

		// 发送数据
		send(cfd, buf, strlen(buf), 0);
		// 清空数组
		memset(buf, 0, sizeof(buf));
		// 释放资源 namelist[i] 这个指针指向一块有效的内存
		free(namelist[i]);
	}
	// 补充html中剩余的标签
	sprintf(buf, "</table></body></html>");
	send(cfd, buf, strlen(buf), 0);
	// 释放namelist
	free(namelist);

	return 0;
}

int disConnect(int cfd, int epfd)
{
	// 将cfd从epoll模型上删除
	int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
	if (ret == -1)
	{
		perror("epoll_ctl");
		close(cfd);
		return -1;
	}
	close(cfd);
	return 0;
}

// 通过文件名获取文件的类型
// 参数: name-> 文件名
// 返回值: 这个文件对应的content-type的类型
const char* getFileType(const char* name)
{
	// a.jpg a.mp4 a.html
	// 自右向左查找‘.’字符, 如不存在返回NULL
	const char* dot = strrchr(name, '.');
	if (dot == NULL)
		return "text/plain; charset=utf-8";	// 纯文本
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(dot, ".gif") == 0)
		return "image/gif";
	if (strcmp(dot, ".png") == 0)
		return "image/png";
	if (strcmp(dot, ".css") == 0)
		return "text/css";
	if (strcmp(dot, ".au") == 0)
		return "audio/basic";
	if (strcmp(dot, ".wav") == 0)
		return "audio/wav";
	if (strcmp(dot, ".avi") == 0)
		return "video/x-msvideo";
	if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
		return "video/quicktime";
	if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
		return "video/mpeg";
	if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
		return "model/vrml";
	if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
		return "audio/midi";
	if (strcmp(dot, ".mp3") == 0)
		return "audio/mpeg";
	if (strcmp(dot, ".ogg") == 0)
		return "application/ogg";
	if (strcmp(dot, ".pac") == 0)
		return "application/x-ns-proxy-autoconfig";

	return "text/plain; charset=utf-8";
}

// 最终得到10进制的整数
int hexit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return 0;
}

// 解码
// from: 要被转换的字符 -> 传入参数
// to: 转换之后得到的字符 -> 传出参数
void decodeMsg(char* to, char* from)
{
	for (; *from != '\0'; ++to, ++from)
	{
		// isxdigit -> 判断字符是不是16进制格式
		// Linux%E5%86%85%E6%A0%B8.jpg
		if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
		{
			// 将16进制的数 -> 十进制 将这个数值赋值给了字符 int -> char
			// A1 == 161
			*to = hexit(from[1]) * 16 + hexit(from[2]);

			from += 2;
		}
		else
		{
			// 不是特殊字符字节赋值
			*to = *from;
		}
	}
	*to = '\0';
}
