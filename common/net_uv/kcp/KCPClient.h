#pragma once

#include "KCPSocket.h"
#include "KCPSession.h"

NS_NET_UV_BEGIN

class KCPClient : public Client
{
protected:

	struct clientSessionData
	{
		clientSessionData() {}
		~clientSessionData() {}
		CONNECTSTATE connectState;
		bool removeTag; // 是否被标记移除
		bool reconnect;	// 是否断线重连
		float curtime;
		float totaltime;
		std::string ip;
		uint32_t port;
		KCPSession* session;
	};

public:

	KCPClient();
	KCPClient(const KCPClient&) = delete;
	virtual ~KCPClient();

	/// Client
	virtual void connect(const char* ip, uint32_t port, uint32_t sessionId)override;

	virtual void closeClient()override;

	virtual void updateFrame()override;

	virtual void removeSession(uint32_t sessionId)override;

	/// SessionManager
	virtual void send(uint32_t sessionId, char* data, uint32_t len)override;

	virtual void disconnect(uint32_t sessionId)override;

	//设置所有socket是否自动重连
	void setAutoReconnect(bool isAuto);

	//设置所有socket自动重连时间(单位：S)
	void setAutoReconnectTime(float time);

	//是否自动重连
	void setAutoReconnectBySessionID(uint32_t sessionID, bool isAuto);

	//自动重连时间(单位：S)
	void setAutoReconnectTimeBySessionID(uint32_t sessionID, float time);

protected:

	/// Runnable
	virtual void run()override;

	/// SessionManager
	virtual void executeOperation()override;

	/// Client
	virtual void onIdleRun()override;

	virtual void onSessionUpdateRun()override;

	/// KCPClient
	void onSocketConnect(Socket* socket, int32_t status);

	void onSessionClose(Session* session);

	void onSessionRecvData(Session* session, char* data, uint32_t len);

	void createNewConnect(void* data);

	void clearData();

	clientSessionData* getClientSessionDataBySessionId(uint32_t sessionId);

	clientSessionData* getClientSessionDataBySession(Session* session);

	void onClientUpdate();

protected:
	uv_timer_t m_clientUpdateTimer;

	bool m_reconnect;		// 是否自动断线重连
	float m_totalTime;		// 断线重连时间

	// 所有会话
	std::map<uint32_t, clientSessionData*> m_allSessionMap;

	bool m_isStop;

protected:

	static void uv_client_update_timer_run(uv_timer_t* handle);
};



NS_NET_UV_END
