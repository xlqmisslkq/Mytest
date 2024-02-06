#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

//原则：main函数只是逻辑调用，具体内容不会写在里面
//功能函数：功能尽可能单一，一般最多几十行
int main(int argc, char* argv[])
{
	//./a.out port path
	if (argc < 3)
	{
		printf("./a.out port path\n");
		exit(0);
	}

	chdir(argv[2]);

	//启动服务器 -> 基于epoll
	unsigned short port = atoi(argv[1]);
	epollRun(port);
	return 0;
}