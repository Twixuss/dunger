#include "..\dep\tl\include\tl\common.h"
#include "..\dep\tl\include\tl\math.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "network.h"
#include <WS2tcpip.h>
#include <winsock2.h>

#include <iostream>
#include <unordered_map>

#pragma comment(lib, "Ws2_32.lib")
using namespace Network;
using namespace TL;
int main(int argc, char** argv) {
	WSADATA wsaData;
	if (auto err = WSAStartup(MAKEWORD(2, 2), &wsaData); err != 0) {
		printf("WSAStartup failed: %d\n", err);
		ASSERT(0);
	}
	DEFER { WSACleanup(); };

	/*
	addrinfo* result = 0;
	addrinfo* ptr = 0;
	addrinfo hints{};

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	if (auto err = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result); err != 0) {
		printf("getaddrinfo failed: %d\n", err);
		ASSERT(0);
	}
	DEFER { freeaddrinfo(result); };
	*/

	SOCKET ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (ListenSocket == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		ASSERT(0);
	}

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(27015);
	addr.sin_addr.S_un.S_addr = INADDR_ANY;
	bind(ListenSocket, (SOCKADDR*)&addr, sizeof(addr));

	listen(ListenSocket, SOMAXCONN);

	DEFER { closesocket(ListenSocket); };
	/*
	if (auto err = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen); err == SOCKET_ERROR) {
		printf("bind failed with error: %d\n", WSAGetLastError());
		ASSERT(0);
	}
	*/

	fd_set master;
	FD_ZERO(&master);
	FD_SET(ListenSocket, &master);

	std::unordered_map<SOCKET, std::vector<ServerMessage>> messages;
	auto send = [&](SOCKET socket, ServerMessage val) { messages[socket].push_back(std::move(val)); };
	auto flush = [&]() {
		for (auto& [s, v] : messages) {
			int iSendResult;
			do {
				iSendResult = ::send(s, (char*)v.data(), v.size() * sizeof(ServerMessage), 0);
			} while (iSendResult <= 0);
		}
		messages.clear();
	};

	struct Player {
		u32 index;
		SOCKET socket;
	};

	std::vector<Player> players;

	auto getPlayerFromSocket = [&players](SOCKET s) -> Player& {
		for (auto& p : players) {
			if (p.socket == s) {
				return p;
			}
		}
		ASSERT(0);
	};
	auto registerPlayer = [&](SOCKET socket) {
		static u32 playerIndex = 0;

		Player newPlayer;
		newPlayer.index = playerIndex++;
		newPlayer.socket = socket;

		printf("index: %u\n", newPlayer.index);
		send(newPlayer.socket, AssignIndex{newPlayer.index});
		for (auto& other : players) {
			send(other.socket, PlayerConnected{newPlayer.index});
			send(newPlayer.socket, PlayerConnected{other.index});
		}
		players.push_back(newPlayer);

		FD_SET(socket, &master);
	};

	/*
	for (;;) {
		int iResult = recv(ClientSocket, recvbuf, DEFAULT_BUFLEN, 0);
		if (iResult > 0) {
			try {
				std::visit(Visitor{[&](PositionChange pos) {
									   for (auto& [k, p] : players) {
										   if (k != pos.playerIndex) {
											   send(pos);
										   }
									   }
								   },
								   [&](BulletFired b) {}},
						   *(Message*)recvbuf);
			} catch (...) {
			}
		} else if (iResult == 0) {
			goto listenAgain;
		} else {
			printf("recv failed: %d\n", WSAGetLastError());
			closesocket(ClientSocket);
			ASSERT(0);
		}
	}
	*/

	for (;;) {
		fd_set copy = master;
		timeval timeout{};
		timeout.tv_sec = 1;
		int socketCount = select(0, &copy, 0, 0, &timeout);
		for (int i = 0; i < socketCount; i++) {
			SOCKET socket = copy.fd_array[i];
			if (socket == ListenSocket) {
				SOCKADDR_IN client;
				SOCKET clientSocket;
				char host[NI_MAXHOST]{};
				char service[NI_MAXSERV]{};
				int clientSize = sizeof(client);
				clientSocket = accept(ListenSocket, (SOCKADDR*)&client, &clientSize);
				if (clientSocket == INVALID_SOCKET) {
					puts("accept failed");
					continue;
				}
				if (getnameinfo((SOCKADDR*)&client, sizeof(client), host, NI_MAXHOST, service, NI_MAXSERV, 0) == 0) {
					std::cout << host << " connected on port " << service << "; ";
				} else {
					inet_ntop(AF_INET, &client.sin_addr, host, NI_MAXHOST);
					std::cout << host << " connected on port " << ntohs(client.sin_port) << "; ";
				}
				registerPlayer(clientSocket);
			} else {
				auto& player = getPlayerFromSocket(socket);
				char recvbuf[sizeof(ClientMessage) * 1024];
				int iResult = recv(socket, recvbuf, _countof(recvbuf), 0);
				if (iResult > 0) {
					if (iResult % sizeof(ClientMessage) != 0) {
						puts("bad messages");
						continue;
					}
					View messages((ClientMessage*)recvbuf, iResult / sizeof(ClientMessage));
					for (auto& m : messages) {
						try {
							std::visit(Visitor{[&](PositionChange pos) {
												   for (auto& p : players) {
													   if (p.socket != socket) {
														   send(p.socket, pos);
													   }
												   }
											   },
											   [&](CreateBullet b) {
												   for (auto& p : players) {
													   if (p.socket != socket) {
														   send(p.socket, b);
													   }
												   }
											   },
											   [&](PlayerHit ph) {
												   for (auto& p : players) {
													   if (p.index == ph.id) {
														   send(p.socket, ph);
														   break;
													   }
												   }
											   },
											   [](auto) {}},
									   m);
						} catch (...) {
							puts("std::visit failed");
						}
					}
				} else if (iResult == 0) {
					std::cout << player.index << " disconnected\n";
					closesocket(socket);
					players.erase(players.begin() + (&player - players.data()));
					for (auto& p : players) {
						send(p.socket, PlayerDisconnected{player.index});
					}
					FD_CLR(socket, &master);
					continue;
				} else {
					int err = WSAGetLastError();
					closesocket(socket);
					if (err == WSAECONNRESET) {
						continue;
					}
					printf("recv failed: %d\n", err);
					ASSERT(0);
				}
			}
		}
		flush();
	}

	system("pause");
}