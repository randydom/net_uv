#include "UDPClient.h"

enum
{
	UDP_CLI_OP_CONNECT,	//	连接
	UDP_CLI_OP_SENDDATA,	// 发送数据
	UDP_CLI_OP_DISCONNECT,	// 断开连接
	UDP_CLI_OP_CLIENT_CLOSE, //客户端退出
};

// 连接操作
struct UDPClientConnectOperation
{
	UDPClientConnectOperation() {}
	~UDPClientConnectOperation() {}
	std::string ip;
	unsigned int port;
	unsigned int sessionID;
};

UDPClient::UDPClient()
	: m_isStop(false)
{
	uv_loop_init(&m_loop);

	uv_idle_init(&m_loop, &m_idle);
	m_idle.data = this;
	uv_idle_start(&m_idle, uv_on_idle_run);

	m_clientStage = clientStage::START;

	this->startThread();
}

UDPClient::~UDPClient()
{
	this->closeClient();
	this->join();
	clearData();
}

/// Client
void UDPClient::connect(const char* ip, unsigned int port, unsigned int sessionId)
{
	if (m_isStop)
		return;

	UDPClientConnectOperation* opData = (UDPClientConnectOperation*)fc_malloc(sizeof(UDPClientConnectOperation));
	new (opData)UDPClientConnectOperation();

	opData->ip = ip;
	opData->port = port;
	opData->sessionID = sessionId;

	pushOperation(UDP_CLI_OP_CONNECT, opData, 0U, 0U);
}

void UDPClient::disconnect(unsigned int sessionId)
{
	if (m_isStop)
		return;

	pushOperation(UDP_CLI_OP_DISCONNECT, NULL, 0U, sessionId);
}

void UDPClient::closeClient()
{
	if (m_isStop)
		return;
	m_isStop = true;
	pushOperation(UDP_CLI_OP_CLIENT_CLOSE, NULL, 0U, 0U);
}

void UDPClient::updateFrame()
{
	if (m_msgMutex.trylock() != 0)
	{
		return;
	}

	if (m_msgQue.empty())
	{
		m_msgMutex.unlock();
		return;
	}

	while (!m_msgQue.empty())
	{
		m_msgDispatchQue.push(m_msgQue.front());
		m_msgQue.pop();
	}
	m_msgMutex.unlock();

	bool closeClientTag = false;
	while (!m_msgDispatchQue.empty())
	{
		const UDPThreadMsg_C& Msg = m_msgDispatchQue.front();
		switch (Msg.msgType)
		{
		case UDPThreadMsgType::RECV_DATA:
		{
			m_recvCall(this, Msg.pSession, Msg.data, Msg.dataLen);
			fc_free(Msg.data);
		}break;
		case UDPThreadMsgType::CONNECT_FAIL:
		{
			if (m_connectCall != nullptr)
			{
				m_connectCall(this, Msg.pSession, false);
			}
		}break;
		case UDPThreadMsgType::CONNECT:
		{
			if (m_connectCall != nullptr)
			{
				m_connectCall(this, Msg.pSession, true);
			}
		}break;
		case UDPThreadMsgType::DIS_CONNECT:
		{
			if (m_disconnectCall != nullptr)
			{
				m_disconnectCall(this, Msg.pSession);
			}
		}break;
		case UDPThreadMsgType::EXIT_LOOP:
		{
			closeClientTag = true;
		}break;
		default:
			break;
		}
		m_msgDispatchQue.pop();
	}
	if (closeClientTag && m_clientCloseCall != nullptr)
	{
		m_clientCloseCall(this);
	}
}

/// SessionManager
void UDPClient::send(Session* session, char* data, unsigned int len)
{
	send(session->getSessionID(), data, len);
}

void UDPClient::disconnect(Session* session)
{
	disconnect(session->getSessionID());
}

/// UDPClient
void UDPClient::send(unsigned int sessionId, char* data, unsigned int len)
{
	if (m_isStop)
		return;

	if (data == 0 || len <= 0)
		return;

	char* sendData = (char*)fc_malloc(len);
	memcpy(sendData, data, len);
	pushOperation(UDP_CLI_OP_SENDDATA, data, len, sessionId);
}

/// Runnable
void UDPClient::run()
{
	m_loop.data = NULL;

	uv_run(&m_loop, UV_RUN_DEFAULT);

	uv_loop_close(&m_loop);

	m_clientStage = clientStage::STOP;
	this->pushThreadMsg(UDPThreadMsgType::EXIT_LOOP, NULL);
}

	/// SessionManager
void UDPClient::executeOperation()
{
	if (m_operationMutex.trylock() != 0)
	{
		return;
	}

	if (m_operationQue.empty())
	{
		m_operationMutex.unlock();
		return;
	}

	while (!m_operationQue.empty())
	{
		m_operationDispatchQue.push(m_operationQue.front());
		m_operationQue.pop();
	}
	m_operationMutex.unlock();

	while (!m_operationDispatchQue.empty())
	{
		auto & curOperation = m_operationDispatchQue.front();
		switch (curOperation.operationType)
		{
		case UDP_CLI_OP_SENDDATA:		// 数据发送
		{
			auto sessionData = getClientSessionDataBySessionId(curOperation.sessionID);
			if (sessionData)
			{
				sessionData->session->executeSend((char*)curOperation.operationData, curOperation.operationDataLen);
			}
			else
			{
				fc_free(curOperation.operationData);
			}
		}break;
		case UDP_CLI_OP_DISCONNECT:	// 断开连接
		{
			auto sessionData = getClientSessionDataBySessionId(curOperation.sessionID);
			if (sessionData->connectState == CONNECT)
			{
				sessionData->session->executeDisconnect();
				sessionData->connectState = DISCONNECT;
			}
		}break;
		case UDP_CLI_OP_CONNECT:	// 连接
		{
			if (curOperation.operationData)
			{
				createNewConnect(curOperation.operationData);
				((UDPClientConnectOperation*)curOperation.operationData)->~UDPClientConnectOperation();
				fc_free(curOperation.operationData);
			}
		}break;
		case UDP_CLI_OP_CLIENT_CLOSE://客户端关闭
		{
			if (m_clientStage == clientStage::START)
			{
				for (auto& it : m_allSessionMap)
				{
					it.second.session->executeDisconnect();
				}
			}
			m_clientStage = clientStage::CLEAR_SESSION;
		}break;
		default:
			break;
		}
		m_operationDispatchQue.pop();
	}
}

void UDPClient::idle_run()
{
	if (m_clientStage == clientStage::CLEAR_SESSION)
	{
		bool hasConnect = false;
		for (auto& it : m_allSessionMap)
		{
			if (it.second.connectState == CONNECT)
			{
				hasConnect = true;
				it.second.session->executeDisconnect();
			}
		}

		if (!hasConnect)
		{
			for (auto& it : m_allSessionMap)
			{
				it.second.session->~KcpSession();
				fc_free(it.second.session);
			}
			m_allSessionMap.clear();
			uv_stop(&m_loop);
		}
	}

	ThreadSleep(1);
}

void UDPClient::clearData()
{
	for (auto& it : m_allSessionMap)
	{
		it.second.session->~KcpSession();
		fc_free(it.second.session);
	}

	m_msgMutex.lock();
	while (!m_msgQue.empty())
	{
		if (m_msgQue.front().data)
		{
			fc_free(m_msgQue.front().data);
		}
		m_msgQue.pop();
	}
	m_msgMutex.unlock();

	while (!m_operationQue.empty())
	{
		auto & curOperation = m_operationQue.front();
		switch (curOperation.operationType)
		{
		case UDP_CLI_OP_SENDDATA:			// 数据发送
		{
			if (curOperation.operationData)
			{
				fc_free(curOperation.operationData);
			}
		}break;
		case UDP_CLI_OP_CONNECT:			// 连接
		{
			if (curOperation.operationData)
			{
				((UDPClientConnectOperation*)curOperation.operationData)->~UDPClientConnectOperation();
				fc_free(curOperation.operationData);
			}
		}break;
		}
		m_operationQue.pop();
	}
}

void UDPClient::pushThreadMsg(UDPThreadMsgType type, Session* session, char* data/* = NULL*/, unsigned int len/* = 0U*/)
{
	UDPThreadMsg_C msg;
	msg.msgType = type;
	msg.data = data;
	msg.dataLen = len;
	msg.pSession = session;

	m_msgMutex.lock();
	m_msgQue.push(msg);
	m_msgMutex.unlock();
}

UDPClient::clientSessionData* UDPClient::getClientSessionDataBySessionId(unsigned int sessionId)
{
	auto it = m_allSessionMap.find(sessionId);
	if (it != m_allSessionMap.end())
	{
		return &it->second;
	}
	return NULL;
}

void UDPClient::createNewConnect(void* data)
{
	if (data == NULL)
		return;

	UDPClientConnectOperation* opData = (UDPClientConnectOperation*)data;

	auto it = m_allSessionMap.find(opData->sessionID);
	if (it != m_allSessionMap.end())
	{
		//对比端口和IP是否一致
		if (strcmp(opData->ip.c_str(), it->second.session->getIp().c_str()) != 0 &&
			opData->port != it->second.session->getPort())
		{
			pushThreadMsg(UDPThreadMsgType::CONNECT_FAIL, NULL);
			return;
		}

		if (it->second.connectState == CONNECTSTATE::DISCONNECT)
		{
			if (it->second.session->getUDPSocket()->connect(opData->ip.c_str(), opData->port))
			{
				it->second.connectState = CONNECTSTATE::CONNECT;
				pushThreadMsg(UDPThreadMsgType::CONNECT, it->second.session);
			}
			else
			{
				it->second.connectState = CONNECTSTATE::DISCONNECT;
				it->second.session->executeDisconnect();
				pushThreadMsg(UDPThreadMsgType::CONNECT_FAIL, it->second.session);
			}
		}
	}
	else
	{
		UDPSocket* socket = (UDPSocket*)fc_malloc(sizeof(UDPSocket));
		new (socket) UDPSocket(&m_loop);
		KcpSession* session = KcpSession::createSession(this, socket, opData->sessionID);
		if (session == NULL)
		{
			NET_UV_LOG(NET_UV_L_FATAL, "创建会话失败，可能是内存不足!!!");
			return;
		}
		session->setSessionID(opData->sessionID);

		clientSessionData cs;
		cs.session = session;

		if (socket->connect(opData->ip.c_str(), opData->port))
		{
			session->setIsOnline(true);
			cs.connectState = CONNECTSTATE::CONNECT;
			pushThreadMsg(UDPThreadMsgType::CONNECT, session);
		}
		else
		{
			session->setIsOnline(false);
			cs.connectState = CONNECTSTATE::DISCONNECT;
			pushThreadMsg(UDPThreadMsgType::CONNECT_FAIL, session);
		}
		m_allSessionMap.insert(std::make_pair(opData->sessionID, cs));
	}
}

//////////////////////////////////////////////////////////////////////////

void UDPClient::uv_on_idle_run(uv_idle_t* handle)
{
	UDPClient* c = (UDPClient*)handle->data;
	c->idle_run();
	ThreadSleep(1);
}

