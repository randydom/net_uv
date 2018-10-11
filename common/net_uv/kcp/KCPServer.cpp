#include "KCPServer.h"
#include "KCPUtils.h"

NS_NET_UV_BEGIN

enum
{
	KCP_SVR_OP_STOP_SERVER,	// ֹͣ������
	KCP_SVR_OP_SEND_DATA,	// ������Ϣ��ĳ���Ự
	KCP_SVR_OP_DIS_SESSION,	// �Ͽ�ĳ���Ự
	KCP_SVR_OP_SEND_DIS_SESSION_MSG_TO_MAIN_THREAD,//�����̷߳��ͻỰ�ѶϿ�
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////

KCPServer::KCPServer()
	: m_start(false)
	, m_server(NULL)
{
	m_serverStage = ServerStage::STOP;
}

KCPServer::~KCPServer()
{
	stopServer();
	this->join();
	clearData();

	NET_UV_LOG(NET_UV_L_INFO, "KCPServer destroy...");
}

void KCPServer::startServer(const char* ip, unsigned int port, bool isIPV6)
{
	if (m_serverStage != ServerStage::STOP)
		return;
	if (m_start)
		return;

	Server::startServer(ip, port, isIPV6);

	m_start = true;
	m_serverStage = ServerStage::START;

	NET_UV_LOG(NET_UV_L_INFO, "KCPServer %s:%d start-up...", ip, port);
	this->startThread();
}

bool KCPServer::stopServer()
{
	if (!m_start)
		return false;
	m_start = false;
	pushOperation(KCP_SVR_OP_STOP_SERVER, NULL, NULL, 0U);
	return true;
}

void KCPServer::updateFrame()
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

	bool closeServerTag = false;
	while (!m_msgDispatchQue.empty())
	{
		const NetThreadMsg& Msg = m_msgDispatchQue.front();

		switch (Msg.msgType)
		{
		case NetThreadMsgType::RECV_DATA:
		{
			m_recvCall(this, Msg.pSession, Msg.data, Msg.dataLen);
			fc_free(Msg.data);
		}break;
		case NetThreadMsgType::START_SERVER_SUC:
		{
			if (m_startCall != nullptr)
			{
				m_startCall(this, true);
			}
		}break;
		case NetThreadMsgType::START_SERVER_FAIL:
		{
			if (m_startCall != nullptr)
			{
				m_startCall(this, false);
			}
		}break;
		case NetThreadMsgType::NEW_CONNECT:
		{
			m_newConnectCall(this, Msg.pSession);
		}break;
		case NetThreadMsgType::DIS_CONNECT:
		{
			m_disconnectCall(this, Msg.pSession);
			pushOperation(KCP_SVR_OP_SEND_DIS_SESSION_MSG_TO_MAIN_THREAD, NULL, 0, Msg.pSession->getSessionID());
		}break;
		case NetThreadMsgType::EXIT_LOOP:
		{
			closeServerTag = true;
		}break;
		default:
			break;
		}
		m_msgDispatchQue.pop();
	}
	if (closeServerTag && m_closeCall != nullptr)
	{
		m_closeCall(this);
	}
}

void KCPServer::send(Session* session, char* data, unsigned int len)
{
	int bufCount = 0;

	uv_buf_t* bufArr = kcp_packageData(data, len, &bufCount);

	if (bufArr == NULL)
		return;

	for (int i = 0; i < bufCount; ++i)
	{
		pushOperation(KCP_SVR_OP_SEND_DATA, (bufArr + i)->base, (bufArr + i)->len, session->getSessionID());
	}
	fc_free(bufArr);
}

void KCPServer::disconnect(Session* session)
{
	pushOperation(KCP_SVR_OP_DIS_SESSION, NULL, 0, session->getSessionID());
}

bool KCPServer::isCloseFinish()
{
	return (m_serverStage == ServerStage::STOP);
}

void KCPServer::run()
{
	int r = uv_loop_init(&m_loop);
	CHECK_UV_ASSERT(r);

	startIdle();
	startSessionUpdate(KCP_HEARTBEAT_TIMER_DELAY);

	m_server = (KCPSocket*)fc_malloc(sizeof(KCPSocket));
	if (m_server == NULL)
	{
		m_serverStage = ServerStage::STOP;
		pushThreadMsg(NetThreadMsgType::START_SERVER_FAIL, NULL);
		return;
	}
	new (m_server) KCPSocket(&m_loop);
	m_server->setCloseCallback(std::bind(&KCPServer::onServerSocketClose, this, std::placeholders::_1));
	m_server->setNewConnectionCallback(std::bind(&KCPServer::onNewConnect, this, std::placeholders::_1));
	m_server->setConnectFilterCallback(std::bind(&KCPServer::onServerSocketConnectFilter, this, std::placeholders::_1));

	bool suc = false;
	if (m_isIPV6)
	{
		suc = m_server->bind6(m_ip.c_str(), m_port);
	}
	else
	{
		suc = m_server->bind(m_ip.c_str(), m_port);
	}

	if (!suc)
	{
		m_server->~KCPSocket();
		fc_free(m_server);
		m_server = NULL;

		m_serverStage = ServerStage::STOP;
		pushThreadMsg(NetThreadMsgType::START_SERVER_FAIL, NULL);
		return;
	}

	suc = m_server->listen();
	if (!suc)
	{
		m_server->~KCPSocket();
		fc_free(m_server);
		m_server = NULL;

		m_serverStage = ServerStage::STOP;
		pushThreadMsg(NetThreadMsgType::START_SERVER_FAIL, NULL);
		return;
	}
	m_serverStage = ServerStage::RUN;
	pushThreadMsg(NetThreadMsgType::START_SERVER_SUC, NULL);

	uv_run(&m_loop, UV_RUN_DEFAULT);

	m_server->~KCPSocket();
	fc_free(m_server);
	m_server = NULL;

	uv_loop_close(&m_loop);

	m_serverStage = ServerStage::STOP;
	pushThreadMsg(NetThreadMsgType::EXIT_LOOP, NULL);
}


void KCPServer::onNewConnect(Socket* socket)
{
	if (socket != NULL)
	{
		KCPSession* session = KCPSession::createSession(this, (KCPSocket*)socket);
		if (session == NULL)
		{
			NET_UV_LOG(NET_UV_L_ERROR, "�����������»Ựʧ��,�������ڴ治��");
		}
		else
		{
			session->setSessionRecvCallback(std::bind(&KCPServer::onSessionRecvData, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
			session->setSessionClose(std::bind(&KCPServer::onSessionClose, this, std::placeholders::_1));
			session->setSendHeartMsg(NET_HEARTBEAT_MSG_S2C);
			session->setHeartMaxCount(KCP_HEARTBEAT_MAX_COUNT_SERVER);
			session->setResetHeartCount(KCP_HEARTBEAT_COUNT_RESET_VALUE_SERVER);
			session->setSessionID(((KCPSocket*)socket)->getConv());
			session->setIsOnline(true);
			addNewSession(session);
		}
	}
	else
	{
		NET_UV_LOG(NET_UV_L_ERROR, "����������ʧ��");
	}
}

void KCPServer::onServerSocketClose(Socket* svr)
{
	m_serverStage = ServerStage::CLEAR;
}

bool KCPServer::onServerSocketConnectFilter(const struct sockaddr* addr)
{
	return true;
}

void KCPServer::addNewSession(KCPSession* session)
{
	if (session == NULL)
	{
		return;
	}
	serverSessionData data;
	data.isInvalid = false;
	data.session = session;

	m_allSession.insert(std::make_pair(session->getSessionID(), data));

	pushThreadMsg(NetThreadMsgType::NEW_CONNECT, session);
}

void KCPServer::onSessionClose(Session* session)
{
	if (session == NULL)
	{
		return;
	}
	auto it = m_allSession.find(session->getSessionID());
	if (it != m_allSession.end())
	{
		it->second.isInvalid = true;
	}

	pushThreadMsg(NetThreadMsgType::DIS_CONNECT, session);
}

void KCPServer::onSessionRecvData(Session* session, char* data, unsigned int len)
{
	pushThreadMsg(NetThreadMsgType::RECV_DATA, session, data, len);
}

void KCPServer::executeOperation()
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
		case KCP_SVR_OP_SEND_DATA:		// ���ݷ���
		{
			auto it = m_allSession.find(curOperation.sessionID);
			if (it != m_allSession.end())
			{
				it->second.session->executeSend((char*)curOperation.operationData, curOperation.operationDataLen);
			}
			else//�ûỰ��ʧЧ
			{
				fc_free(curOperation.operationData);
			}
		}break;
		case KCP_SVR_OP_DIS_SESSION:	// �Ͽ�����
		{
			auto it = m_allSession.find(curOperation.sessionID);
			if (it != m_allSession.end())
			{
				it->second.session->executeDisconnect();
			}
		}break;
		case KCP_SVR_OP_SEND_DIS_SESSION_MSG_TO_MAIN_THREAD:
		{
			auto it = m_allSession.find(curOperation.sessionID);
			if (it != m_allSession.end())
			{
				it->second.session->~KCPSession();
				fc_free(it->second.session);
				it = m_allSession.erase(it);
			}
		}break;
		case KCP_SVR_OP_STOP_SERVER:
		{
			for (auto & it : m_allSession)
			{
				if (!it.second.isInvalid)
				{
					it.second.session->executeDisconnect();
				}
			}
			m_server->disconnect();
			m_serverStage = ServerStage::WAIT_CLOSE_SERVER_SOCKET;

			stopSessionUpdate();
		}break;
		default:
			break;
		}
		m_operationDispatchQue.pop();
	}
}

void KCPServer::clearData()
{
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
		if (m_operationQue.front().operationType == KCP_SVR_OP_SEND_DATA)
		{
			fc_free(m_operationQue.front().operationData);
		}
		m_operationQue.pop();
	}
}

void KCPServer::onIdleRun()
{
	executeOperation();

	switch (m_serverStage)
	{
	case KCPServer::ServerStage::CLEAR:
	{
		for (auto& it : m_allSession)
		{
			if (!it.second.isInvalid)
			{
				it.second.session->executeDisconnect();
			}
		}
		m_serverStage = ServerStage::WAIT_SESSION_CLOSE;
	}
	break;
	case KCPServer::ServerStage::WAIT_SESSION_CLOSE:
	{
		if (m_allSession.empty())
		{
			stopIdle();
			uv_stop(&m_loop);
			m_serverStage = ServerStage::STOP;
		}
	}
	break;
	default:
		break;
	}
	ThreadSleep(1);
}

void KCPServer::onSessionUpdateRun()
{
	for (auto& it : m_allSession)
	{
		it.second.session->update(KCP_HEARTBEAT_TIMER_DELAY);
	}
}

NS_NET_UV_END