#pragma once
#include "stdafx.h"
#include <thread>
#include "Netplay/GameServerConnection.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Shared/Interfaces/IInputProvider.h"
#include "Shared/Interfaces/IInputRecorder.h"

using std::thread;
class Emulator;

class GameServer : public IInputRecorder, public IInputProvider, public INotificationListener
{
private:
	static shared_ptr<GameServer> Instance;
	shared_ptr<Emulator> _emu;
	unique_ptr<thread> _serverThread;
	atomic<bool> _stop;
	unique_ptr<Socket> _listener;
	uint16_t _port = 0;
	string _password;
	list<shared_ptr<GameServerConnection>> _openConnections;
	bool _initialized = false;

	uint8_t _hostControllerPort = 0;

	void AcceptConnections();
	void UpdateConnections();

	void Exec();
	void Stop();

public:
	GameServer(shared_ptr<Emulator> emu, uint16_t port, string password);
	virtual ~GameServer();

	void RegisterServerInput();

	static void StartServer(shared_ptr<Emulator> emu, uint16_t port, string password);
	static void StopServer();
	static bool Started();

	static uint8_t GetHostControllerPort();
	static void SetHostControllerPort(uint8_t port);
	static uint8_t GetAvailableControllers();
	static vector<PlayerInfo> GetPlayerList();
	static void SendPlayerList();

	static list<shared_ptr<GameServerConnection>> GetConnectionList();

	bool SetInput(BaseControlDevice *device) override;
	void RecordInput(vector<shared_ptr<BaseControlDevice>> devices) override;

	// Inherited via INotificationListener
	virtual void ProcessNotification(ConsoleNotificationType type, void * parameter) override;
};