#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

//ԭ��main����ֻ���߼����ã��������ݲ���д������
//���ܺ��������ܾ����ܵ�һ��һ����༸ʮ��
int main(int argc, char* argv[])
{
	//./a.out port path
	if (argc < 3)
	{
		printf("./a.out port path\n");
		exit(0);
	}

	chdir(argv[2]);

	//���������� -> ����epoll
	unsigned short port = atoi(argv[1]);
	epollRun(port);
	return 0;
}