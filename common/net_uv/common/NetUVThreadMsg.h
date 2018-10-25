#pragma once

#include "../base/Macros.h"

NS_NET_UV_BEGIN

//消息类型
enum class NetThreadMsgType
{
	START_SERVER_SUC,	//服务器启动成功
	START_SERVER_FAIL,	//服务器启动失败
	CONNECT_FAIL,		//连接失败
	CONNECT_TIMOUT,		//连接超时
	CONNECT_SESSIONID_EXIST,//会话ID已存在，且新连接IP和端口和之前会话不一致
	CONNECT_ING,		//正在连接
	CONNECT,			//连接成功
	NEW_CONNECT,		//新连接
	DIS_CONNECT,		//断开连接
	EXIT_LOOP,			//退出loop
	RECV_DATA,			//收到消息
	REMOVE_SESSION,		//移除会话
	CREATE_PRE_SOCKET, // 创建预制socket
};

class Session;
struct NetThreadMsg
{
	NetThreadMsgType msgType;
	Session* pSession;
	char* data;
	unsigned int dataLen;
};

NS_NET_UV_END
