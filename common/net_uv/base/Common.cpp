#include "Common.h"
#include "Misc.h"

#if OPEN_NET_MEM_CHECK == 1
#include "Mutex.h"
#endif

NS_NET_UV_BEGIN

typedef void(*uvOutputLoggerType)(int, const char*);
uvOutputLoggerType uvOutputLogger = 0;


static const char* net_uv_log_name[NET_UV_L_FATAL + 1] =
{
	"HEART",
	"INFO",
	"WARNING",
	"ERROR",
	"FATAL"
};

void net_uvLog(int level, const char* format, ...)
{
	if (level < NET_UV_L_MIN_LEVEL)
	{
		return;
	}

	va_list args;
	char buf[1024];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	std::string str = net_getTime();
	str.append("[NET-UV]-[");
	str.append(net_uv_log_name[level]);
	str.append("] ");
	str.append(buf);
	str.append("\n");
	if (uvOutputLogger == NULL)
	{
		printf("%s", str.c_str());
	}
	else
	{
		uvOutputLogger(level, buf);
	}

	//va_list list;
	//va_start(list, format);
	//vprintf(format, list);
	//va_end(list);
	//printf("\n");
}

void setNetUVLogPrintFunc(void(*func)(int, const char*))
{
	uvOutputLogger = func;
}




///////////////////////////////////////////////////////////////////////////////////////////////////
#if OPEN_NET_MEM_CHECK == 1
struct mallocBlockInfo
{
	unsigned int len;
	std::string file;
	int line;
};

Mutex block_mutex;
unsigned int block_size = 0;
std::map<void*, mallocBlockInfo> block_map;

void* fc_malloc_s(unsigned int len, const char* file, int line)
{
	mallocBlockInfo info;
	info.file = file;
	info.line = line;
	info.len = len;

	void* p = malloc(len);

	if (p == NULL)
	{
		NET_UV_LOG(NET_UV_L_FATAL, "�����ڴ�ʧ��!!!");
#if defined (WIN32) || defined(_WIN32)
		MessageBox(NULL, TEXT("�����ڴ�ʧ��!!!"), TEXT("ERROR"), MB_OK);
		assert(0);
#else
		printf("�����ڴ�ʧ��!!!\n");
		assert(0);
#endif
		return NULL;
	}

	block_mutex.lock();

	block_size++;
	block_map.insert(std::make_pair(p, info));

	block_mutex.unlock();

	return p;
}

void fc_free(void* p)
{
	if (p == NULL)
	{
		assert(0);
		return;
	}
	block_mutex.lock();

	auto it = block_map.find(p);
	if (it == block_map.end())
	{
		assert(p == NULL);
		NET_UV_LOG(NET_UV_L_WARNING, "fc_free: [%p] not find", p);
	}
	else
	{
		block_size--;
		block_map.erase(it);
	}
	free(p);

	block_mutex.unlock();
}



void printMemInfo()
{
	block_mutex.lock();
	NET_UV_LOG(NET_UV_L_INFO, "block size = %d\n", block_size);
	auto it = block_map.begin();
	for (; it != block_map.end(); ++it)
	{
		NET_UV_LOG(NET_UV_L_INFO, "[%p] : [%d] [%s  %d]\n", it->first, it->second.len, it->second.file.c_str(), it->second.line);
	}
	block_mutex.unlock();
}

#else
#endif

NS_NET_UV_END

