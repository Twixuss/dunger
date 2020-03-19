#pragma once
#include "..\dep\tl\include\tl\common.h"
#include "..\dep\tl\include\tl\math.h"
#include "..\dep\tl\include\tl\thread.h"

#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>

#include <array>
#include <variant>
using namespace TL;
inline static i64 getPerfCounter() {
	LARGE_INTEGER r;
	QueryPerformanceCounter(&r);
	return r.QuadPart;
}
inline static i64 const perfFrequency = [] {
	LARGE_INTEGER r;
	QueryPerformanceFrequency(&r);
	return r.QuadPart;
}();
inline static f32 getPerfSeconds(i64 begin, i64 end) { return f32(end - begin) / perfFrequency; }
inline static i32 randomSeed = getPerfCounter();
inline static f32 randomF32() { return (randomSeed = randomize(randomSeed) / 256) * (1.0f / 8388608.f); }
inline static bool randomBool() { return (randomSeed = randomize(randomSeed)) & 0x10; }
inline static i32 randomI32() { return randomSeed = randomize(randomSeed); }
#define CHUNK_W 32
struct Tile {
	bool exists;
};
using Tiles = std::array<std::array<Tile, CHUNK_W>, CHUNK_W>;
inline static int const midRadius = 5;
inline static Tiles generateMap() {
	Tiles tiles;
	for (int x = 0; x < CHUNK_W; ++x) {
		tiles[x][0].exists = true;
		tiles[x][CHUNK_W - 1].exists = true;
	}
	for (int y = 0; y < CHUNK_W; ++y) {
		tiles[0][y].exists = true;
		tiles[CHUNK_W - 1][y].exists = true;
	}
	for (int x = 1; x < CHUNK_W - 1; ++x) {
		for (int y = 1; y < CHUNK_W - 1; ++y) {
			tiles[x][y].exists = !(randomI32() & 0x70);
		}
	}
	for (int x = 1; x < 3; ++x) {
		for (int y = 1; y < 3; ++y) {
			tiles[x][y].exists = false;
			tiles[CHUNK_W - 1 - x][y].exists = false;
			tiles[CHUNK_W - 1 - x][CHUNK_W - 1 - y].exists = false;
			tiles[x][CHUNK_W - 1 - y].exists = false;
		}
	}
	for (int x = -midRadius; x < midRadius; ++x) {
		for (int y = -midRadius; y < midRadius; ++y) {
			if (abs(x) + abs(y) <= midRadius)
				tiles[x + CHUNK_W / 2][y + CHUNK_W / 2].exists = false;
		}
	}
	return tiles;
}
namespace Network {
struct PositionChange {
	u32 playerIndex;
	v2 position;
};
struct PlayerConnected {
	u32 playerIndex;
};
struct PlayerDisconnected {
	u32 playerIndex;
};
struct AssignIndex {
	u32 newIndex;
};
struct PlayerHit {
	u32 id;
};
struct CreateBullet {
	u32 id;
	v2 position;
	v2 direction;
};
struct GetTiles {
	Tiles tiles;
};
u32 makeBulletID(u32 playerId, u32 bulletId) { return (playerId << 24) | (bulletId & 0xFFFFFF); }
u32 getPlayerIdFromBulletId(u32 bulletId) { return bulletId >> 24; }
using ClientMessage = std::variant<PositionChange, CreateBullet, PlayerHit>;
using ServerMessage =
	std::variant<AssignIndex, PositionChange, CreateBullet, PlayerHit, GetTiles, PlayerConnected, PlayerDisconnected>;
} // namespace Network