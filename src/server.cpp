#include "..\dep\tl\include\tl\common.h"
#include "..\dep\tl\include\tl\math.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shared.h"
#include <WS2tcpip.h>
#include <winsock2.h>

#include <iostream>
#include <mutex>
#include <queue>
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

	fd_set master;
	FD_ZERO(&master);
	FD_SET(ListenSocket, &master);

	static Tiles tiles = generateMap();

	struct Player {
		u32 id;
		SOCKET socket;
		v2 position;
		f32 invulnerableTime = 1.0f;
		u32 health = 5;
	};

	static std::vector<Player> players;

	static std::queue<Player> newPlayers;
	static std::mutex newPlayersMutex;

	static std::queue<SOCKET> disconnectedPlayers;
	static std::mutex disconnectedPlayersMutex;

	auto pushNewPlayer = [&](Player&& p) {
		newPlayersMutex.lock();
		newPlayers.push(std::move(p));
		newPlayersMutex.unlock();
	};
	static auto pushDisconnectedPlayer = [&](SOCKET s) {
		disconnectedPlayersMutex.lock();
		disconnectedPlayers.push(std::move(s));
		disconnectedPlayersMutex.unlock();
	};

	static auto getPlayerFromSocket = [&](SOCKET s) -> Player* {
		for (auto& p : players) {
			if (p.socket == s) {
				return &p;
			}
		}
		return 0;
	};
	static auto getPlayerFromID = [&](u32 id) -> Player& {
		for (auto& p : players) {
			if (p.id == id) {
				return p;
			}
		}
		ASSERT(0);
	};
	auto registerPlayer = [&](SOCKET socket) {};

	struct Message {
		Player& sender;
		ClientMessage msg;
	};

	static SPSC::CircularQueue<Message, 1024> networkQueue;

	CloseHandle(CreateThread(
		0, 0,
		[](void*) -> DWORD {
			struct {
				f32 const targetFrameTime = 1.0f / 64.0f;
				f32 delta = 0.0f;
				f32 time = 0.0f;
				u32 frameCount = 0;
			} time;

			struct Bullet {
				u32 creator;
				u32 id;
				v2 position;
				v2 direction;
				f32 remainingLifeTime = 5.0f;
			};

			std::vector<Bullet> bullets;

			std::unordered_map<SOCKET, std::vector<ServerMessage>> messages;
			auto send = [&](SOCKET socket, ServerMessage val) { messages[socket].push_back(std::move(val)); };

			auto popNewPlayer = [&]() -> std::optional<Player> {
				newPlayersMutex.lock();
				std::optional<Player> result;
				if (newPlayers.size()) {
					result.emplace(std::move(newPlayers.front()));
					newPlayers.pop();
				}
				newPlayersMutex.unlock();
				return result;
			};
			auto popDisconnectedPlayer = [&]() -> std::optional<SOCKET> {
				disconnectedPlayersMutex.lock();
				std::optional<SOCKET> result;
				if (disconnectedPlayers.size()) {
					result.emplace(std::move(disconnectedPlayers.front()));
					disconnectedPlayers.pop();
				}
				disconnectedPlayersMutex.unlock();
				return result;
			};

			auto lastPerfCounter = getPerfCounter();
			// main loop
			for (;;) {
				// register new players
				for (;;) {
					if (auto opt = popNewPlayer()) {
						auto& newPlayer = *opt;
						printf("id: %u\n", newPlayer.id);
						send(newPlayer.socket, AssignId{newPlayer.id});
						send(newPlayer.socket, GetTiles{tiles});
						for (auto& other : players) {
							send(newPlayer.socket, PlayerConnected{other.id, other.health});
							send(other.socket, PlayerConnected{newPlayer.id, newPlayer.health});
						}
						players.push_back(newPlayer);

					} else {
						break;
					}
				}
				// handle disconnected players
				for (;;) {
					if (auto opt = popDisconnectedPlayer()) {
						auto ptr = getPlayerFromSocket(*opt);
						if (!ptr)
							continue;
						auto& disconnectedPlayer = *ptr;
						printf("%u disconnected\n", disconnectedPlayer.id);
						players.erase(players.begin() + (&disconnectedPlayer - players.data()));
						for (auto& p : players) {
							send(p.socket, PlayerDisconnected{disconnectedPlayer.id});
						}
					} else {
						break;
					}
				}
				// get network messages
				for (;;) {
					if (auto opt = networkQueue.pop()) {
						auto& msg = *opt;
						try {
							std::visit(
								Visitor{[&](ChangePosition pos) {
											msg.sender.position = pos.position;
											for (auto& p : players) {
												if (p.socket != msg.sender.socket) {
													send(p.socket, ChangeEnemyPosition{msg.sender.id, pos.position});
												}
											}
										},
										[&](CreateBullet b) {
											printf("%u shoots\n", msg.sender.id);
											Bullet nb;
											nb.creator = b.creator;
											nb.id = b.id;
											nb.position = b.position;
											nb.direction = b.direction;
											bullets.push_back(nb);
											for (auto& p : players) {
												if (p.socket != msg.sender.socket) {
													send(p.socket, b);
												}
											}
										}},
								msg.msg);
						} catch (...) {
							puts("std::visit failed");
						}
					} else {
						break;
					}
				}
				for (auto& p : players) {
					p.invulnerableTime -= time.delta;
				}
				auto raycastBullet = [&](u32 firingPlayerId, u32 bulletId, v2 a, v2 b) {
					Hit hit{};
					v2 tMin, tMax;
					minmax(a, b, tMin, tMax);
					v2i tMini = max(roundInt(tMin), V2i(0));
					v2i tMaxi = min(roundInt(tMax) + 1, V2i(CHUNK_W));
					for (i32 tx = tMini.x; tx < tMaxi.x; ++tx) {
						for (i32 ty = tMini.y; ty < tMaxi.y; ++ty) {
							ASSERT(tx < CHUNK_W);
							ASSERT(ty < CHUNK_W);
							if (!tiles.get(tx, ty))
								continue;
							v2 normal;
							if (hit = raycastTile(a, b, V2(tx, ty), 0.5f); hit.hit) {
								return hit;
							}
						}
					}
					for (auto& target : players) {
						if (target.id == firingPlayerId || target.invulnerableTime >= 0) {
							continue;
						}
						if (hit = raycastCircle(a, b, target.position, playerRadius); hit.hit) {
							send(getPlayerFromID(firingPlayerId).socket, EnemyHit{target.id, target.health});
							if (--target.health == 0) {
								target.health = 5;
								v2 newPos = getRandomPosition(tiles);
								send(target.socket, ChangePosition{newPos});
								target.position = newPos;
								target.invulnerableTime = 1;
								for (auto& p : players) {
									if (p.id != target.id) {
										send(p.socket, ChangeEnemyPosition{p.id, newPos});
									}
								}
								send(getPlayerFromID(firingPlayerId).socket, EnemyKill{target.id});
							}
							send(target.socket, HealthChange{target.health});
							for (auto& p : players) {
								send(p.socket, ExplodeBullet{firingPlayerId, bulletId, hit.normal});
							}
							return hit;
						}
					}
					return hit;
				};
				for (u32 i = 0; i < bullets.size(); ++i) {
					auto& b = bullets[i];
					b.remainingLifeTime -= time.delta;
					auto destroyBullet = [&] {
						bullets.erase(bullets.begin() + i);
						--i;
					};
					if (b.remainingLifeTime <= 0) {
						destroyBullet();
						continue;
					}
					v2 nextPos = b.position + b.direction * time.delta * 10;
					if (auto hit = raycastBullet(b.creator, b.id, b.position, nextPos); hit.hit) {
						destroyBullet();
						continue;
					}
					b.position = nextPos;
				}

				auto secondsElapsed = getPerfSeconds(lastPerfCounter, getPerfCounter());
				if (secondsElapsed < time.targetFrameTime) {
					i32 msToSleep = (i32)((time.targetFrameTime - secondsElapsed) * 1000.0f);
					if (msToSleep > 0) {
						Sleep((DWORD)msToSleep);
					}
					auto targetCounter = lastPerfCounter + i64(time.targetFrameTime * perfFrequency);
					while (getPerfCounter() < targetCounter)
						;
				}
				auto endCounter = getPerfCounter();
				time.delta = getPerfSeconds(lastPerfCounter, endCounter);
				lastPerfCounter = endCounter;
				time.time += time.delta;
				++time.frameCount;

				for (auto& [s, v] : messages) {
					if (v.size() == 0)
						continue;
					for (;;) {
						int result = ::send(s, (char*)v.data(), v.size() * sizeof(v[0]), 0);
						if (result > 0)
							break;
						int error = WSAGetLastError();
						if (error == WSAENOTSOCK) {
							pushDisconnectedPlayer(s);
							break;
						}
						printf("send failed: %i\n", error);
						ASSERT(0);
					}
				}
				messages.clear();
			}
			return 0;
		},
		0, 0, 0));

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
				FD_SET(clientSocket, &master);
				static u32 playerIndex = 0;
				Player p;
				p.socket = clientSocket;
				p.id = playerIndex++;
				pushNewPlayer(std::move(p));
			} else {
				auto& player = *getPlayerFromSocket(socket);
				char recvbuf[sizeof(ClientMessage) * 1024];
				int iResult = recv(socket, recvbuf, _countof(recvbuf), 0);
				if (iResult > 0) {
					if (iResult % sizeof(ClientMessage) != 0) {
						puts("bad messages");
						continue;
					}
					View messages((ClientMessage*)recvbuf, iResult / sizeof(ClientMessage));
					for (auto& m : messages) {
						networkQueue.push({player, std::move(m)});
					}
				} else if (iResult == 0) {
					closesocket(socket);
					pushDisconnectedPlayer(socket);
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
	}

	system("pause");
}