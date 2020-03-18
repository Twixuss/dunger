#pragma once
#include "D:\src\tl\include\tl\common.h"
#include "D:\src\tl\include\tl\math.h"
#include "D:\src\tl\include\tl\thread.h"
#include <variant>
using namespace TL;
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
u32 makeBulletID(u32 playerId, u32 bulletId) {
	return (playerId << 24) | (bulletId & 0xFFFFFF);
}
u32 getPlayerIdFromBulletId(u32 bulletId) {
	return bulletId >> 24;
}
using ClientMessage = std::variant<PositionChange, CreateBullet, PlayerHit>;
using ServerMessage = std::variant<AssignIndex, PositionChange, CreateBullet, PlayerHit, PlayerConnected, PlayerDisconnected>;
}