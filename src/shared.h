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
struct Tiles {
	u32 data[CHUNK_W];
	void set(bool v, u32 x, u32 y) {
		ASSERT(x < CHUNK_W);
		ASSERT(y < CHUNK_W);
		if (v) {
			data[x] |= (1 << y);
		} else {
			data[x] &= ~(1 << y);
		}
	}
	bool get(u32 x, u32 y) const { 
		ASSERT(x < CHUNK_W);
		ASSERT(y < CHUNK_W);
		return data[x] & (1 << y); 
	}
};
inline static f32 const playerRadius = 0.45f;
inline static int const midRadius = 5;
inline static Tiles generateMap() {
	Tiles tiles;
	for (int x = 0; x < CHUNK_W; ++x) {
		tiles.set(true, x, 0);
		tiles.set(true, x, CHUNK_W - 1);
	}
	for (int y = 0; y < CHUNK_W; ++y) {
		tiles.set(true, 0, y);
		tiles.set(true, CHUNK_W - 1, y);
	}
	for (int x = 1; x < CHUNK_W - 1; ++x) {
		for (int y = 1; y < CHUNK_W - 1; ++y) {
			tiles.set(!(randomI32() & 0b11110), x, y);
		}
	}
	for (int x = 1; x < 3; ++x) {
		for (int y = 1; y < 3; ++y) {
			tiles.set(false, x, y);
			tiles.set(false, CHUNK_W - 1 - x, y);
			tiles.set(false, CHUNK_W - 1 - x, CHUNK_W - 1 - y);
			tiles.set(false, x, CHUNK_W - 1 - y);
		}
	}
	for (int x = -midRadius; x < midRadius; ++x) {
		for (int y = -midRadius; y < midRadius; ++y) {
			if (abs(x) + abs(y) <= midRadius)
				tiles.set(false, x + CHUNK_W / 2, y + CHUNK_W / 2);
		}
	}
	return tiles;
}
struct Hit {
	bool hit = false;
	v2 point;
	v2 normal;
};
Hit raycastLine(v2 a, v2 b, v2 c, v2 d) {
	Hit hit;

	v2 s1 = b - a;
	v2 s2 = d - c;

	v2 st = v2{s1.x, s2.x} * (a.y - c.y) - v2{s1.y, s2.y} * (a.x - c.x);
	st /= s1.x * s2.y - s2.x * s1.y;

	if (st.x >= 0 && st.x <= 1 && st.y >= 0 && st.y <= 1) {
		hit.point = a + (st.y * s1);
		hit.normal = c - d;
		hit.normal = cross(hit.normal);
		if (dot(b - a, hit.normal) > 0)
			hit.normal *= -1;
		hit.hit = true;
	}
	return hit;
}
Hit raycastTile(v2 a, v2 b, v2 tile, float size) {
	Hit hit;
	f32 const w = size;
	// clang-format off
			Hit hits[]{
				raycastLine(a, b, tile + v2{-w, w}, tile + v2{ w, w}),
				raycastLine(a, b, tile + v2{ w, w}, tile + v2{ w,-w}),
				raycastLine(a, b, tile + v2{ w,-w}, tile + v2{-w,-w}),
				raycastLine(a, b, tile + v2{-w,-w}, tile + v2{-w, w}),
			};
	// clang-format on
	f32 minDist = FLT_MAX;
	int minIndex = -1;
	for (int i = 0; i < _countof(hits); ++i) {
		if (!hits[i].hit)
			continue;
		f32 len = lengthSqr(a - hits[i].point);
		if (len < minDist) {
			minDist = len;
			minIndex = i;
		}
	}
	if (minIndex == -1) {
		hit.hit = false;
	} else {
		hit = hits[minIndex];
	}
	return hit;
}
Hit raycastCircle(v2 a, v2 b, v2 circle, f32 radius) {
	Hit hit;
	int intersections = -1;
	v2 intersection1;
	v2 intersection2;

	v2 d = b - a;

	f32 A = lengthSqr(d);
	f32 B = 2 * dot(d, a - circle);

	f32 det = B * B - 4 * A * (lengthSqr(a - circle) - radius * radius);
	if ((A <= 0.0000001) || (det < 0)) {
		// No real solutions.
		intersection1 = {NAN, NAN};
		intersection2 = {NAN, NAN};
		intersections = 0;
	} else if (det == 0) {
		// One solution.
		f32 t = -B / (2 * A);
		intersection1 = a + t * d;
		intersection2 = {NAN, NAN};
		intersections = 1;
	} else {
		// Two solutions.
		f32 s = sqrtf(det);
		intersection1 = a + ((-B + s) / (2 * A)) * d;
		intersection2 = a + ((-B - s) / (2 * A)) * d;
		intersections = 2;
	}

	hit.hit = intersections > 0;
	if (intersections == 1) {
		hit.point = intersection1; // one intersection
	} else if (intersections == 2) {
		f32 dist1 = distanceSqr(intersection1, a);
		f32 dist2 = distanceSqr(intersection2, a);

		if (dist1 < dist2)
			hit.point = intersection1;
		else
			hit.point = intersection2;
	}
	if (hit.hit) {
		if (distanceSqr(a, hit.point) > distanceSqr(a, b)) {
			hit.hit = false;
		} else {
			hit.normal = normalize(circle - hit.point);
		}
	}

	return hit; // no intersections at all
}

namespace Network {
struct ChangePosition {
	v2 position;
};
struct ChangeEnemyPosition {
	u32 id;
	v2 position;
};
struct PlayerConnected {
	u32 id;
	u32 health;
};
struct PlayerDisconnected {
	u32 id;
};
struct AssignId {
	u32 id;
};
struct EnemyHit {
	u32 id;
	u32 health;
};
struct EnemyKill {
	u32 id;
};
struct HealthChange {
	u32 health;
};
struct CreateBullet {
	u32 creator;
	u32 id;
	v2 position;
	v2 direction;
};
struct ExplodeBullet {
	u32 creator;
	u32 id;
	v2 normal;
};
struct GetTiles {
	Tiles tiles;
};
using ClientMessage = std::variant<ChangePosition, CreateBullet>;
using ServerMessage =
	std::variant<AssignId, ChangePosition, ChangeEnemyPosition, CreateBullet, ExplodeBullet, EnemyHit, EnemyKill, HealthChange, GetTiles, PlayerConnected, PlayerDisconnected>;
} // namespace Network