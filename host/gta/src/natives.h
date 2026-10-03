// The GTA V natives the host script calls, by hash. Names, hashes and parameters are those of alloc8or's native DB
// (github.com/alloc8or/gta5-nativedb-data); ScriptHookV translates the original PC hashes for the running build.
#pragma once

#include <types.h>
#include <nativeCaller.h>

namespace natives
{
	inline Ped PlayerPedId() { return invoke<Ped>(0xD80958FC74E988A6); }
	inline Player PlayerId() { return invoke<Player>(0x4F8644AF03D0E0D6); }
	inline Vector3 GetEntityCoords(Entity e, BOOL alive) { return invoke<Vector3>(0x3FEF770D40960D5A, e, alive); }
	inline float GetEntityHeading(Entity e) { return invoke<float>(0xE83D4F9BA2A38914, e); }
	inline void SetEntityCoordsNoOffset(Entity e, float x, float y, float z) { invoke<Void>(0x239A3351AC1DA385, e, x, y, z, FALSE, FALSE, FALSE); }
	inline void SetEntityHeading(Entity e, float heading) { invoke<Void>(0x8E2530AA8ADA980E, e, heading); }
	inline void FreezeEntityPosition(Entity e, BOOL toggle) { invoke<Void>(0x428CA6DBD1094446, e, toggle); }
	inline void SetEntityVisible(Entity e, BOOL toggle) { invoke<Void>(0xEA1C610A04DB6BBB, e, toggle, FALSE); }
	inline void SetEntityCollision(Entity e, BOOL toggle, BOOL keepPhysics) { invoke<Void>(0x1A9205C1B9EE827F, e, toggle, keepPhysics); }
	inline void SetEntityAlpha(Entity e, int alpha) { invoke<Void>(0x44A0870B7E92D7C0, e, alpha, FALSE); }
	inline void ResetEntityAlpha(Entity e) { invoke<Void>(0x9B1E824FFBB7027A, e); }
	inline void SetEntityInvincible(Entity e, BOOL toggle) { invoke<Void>(0x3882114BDE571AD4, e, toggle, FALSE); }
	inline void SetPlayerInvincible(Player p, BOOL toggle) { invoke<Void>(0x239528EACDC3E7DE, p, toggle); }
	inline Vector3 GetOffsetFromEntityGivenWorldCoords(Entity e, float x, float y, float z) { return invoke<Vector3>(0x2274BC1C4885E333, e, x, y, z); }
	inline void SetVehicleDamage(Vehicle v, float x, float y, float z, float damage, float radius) { invoke<Void>(0xA1DD317EA8FD4F29, v, x, y, z, damage, radius, TRUE); }
	/// Where a line through GTA's world first meets its map, vehicles, people or objects, and which of them it met (0 for
	/// the map). False when it meets nothing.
	inline bool LineHits(float x1, float y1, float z1, float x2, float y2, float z2, Entity ignored, Vector3 &at, Vector3 &normal, Entity &entity)
	{
		const int test = invoke<int>(0x377906D8A31E5586, x1, y1, z1, x2, y2, z2, 1 | 2 | 4 | 8 | 16, ignored, 7);
		BOOL hit = FALSE;
		entity = 0;
		invoke<int>(0x3D87450E15D98694, test, &hit, &at, &normal, &entity);
		return hit != FALSE;
	}
	inline void HideHudComponentThisFrame(int component) { invoke<Void>(0x6806C51AD12B83B8, component); }
	inline void DisplayRadar(BOOL shown) { invoke<Void>(0xA0EBB943C300E693, shown); }
	inline void TaskGoStraightToCoord(Ped p, float x, float y, float z, float speed, int milliseconds, float heading)
	{
		invoke<Void>(0xD76B57B44F1E6F8B, p, x, y, z, speed, milliseconds, heading, 0.2f);
	}
	inline void GiveWeaponToPed(Ped p, Hash weapon) { invoke<Void>(0xBF0FD6E56C964FCB, p, weapon, 9999, FALSE, TRUE); }
	inline void SetCurrentPedWeapon(Ped p, Hash weapon) { invoke<Void>(0xADF692B254977C0C, p, weapon, TRUE); }
	inline Entity GetCurrentPedWeaponEntityIndex(Ped p) { return invoke<Entity>(0x3B390A939AF0B5FC, p, FALSE); }
	inline void SetCursorPosition(float x, float y) { invoke<Void>(0xFC695459D4D0E219, x, y); }
	inline BOOL IsDisabledControlPressed(int group, int control) { return invoke<BOOL>(0xE2587F8CBBD87B1D, group, control); }
	inline BOOL IsControlPressed(int group, int control) { return invoke<BOOL>(0xF3A21BCD95725A4A, group, control); }
	inline void SetPedCanRagdoll(Ped p, BOOL can) { invoke<Void>(0xB128377056A54E2A, p, can); }
	inline void ClearPedTasks(Ped p) { invoke<Void>(0xE1EF3C1216AFF2CD, p); }
	inline void ClearPedTasksImmediately(Ped p) { invoke<Void>(0xAAA34F8A7CB32098, p); }
	inline Vehicle CreateVehicle(Hash model, float x, float y, float z, float heading) { return invoke<Vehicle>(0xAF35D0D2583051B0, model, x, y, z, heading, FALSE, FALSE, FALSE); }
	inline Object CreateObjectNoOffset(Hash model, float x, float y, float z) { return invoke<Object>(0x9A294B2138ABB884, model, x, y, z, FALSE, FALSE, TRUE, FALSE); }
	inline Ped ClonePed(Ped p) { return invoke<Ped>(0xEF29A16337FACADB, p, FALSE, FALSE, TRUE); }
	inline void SetEntityAsMissionEntity(Entity e) { invoke<Void>(0xAD738C3085FE7E11, e, TRUE, TRUE); }
	inline void DeleteEntity(Entity *e) { invoke<Void>(0xAE3CBE5BF394C9C9, e); }
	inline void SetEntityAsNoLongerNeeded(Entity *e) { invoke<Void>(0xB736A491E64A32CF, e); }
	inline void SetEntityQuaternion(Entity e, float x, float y, float z, float w) { invoke<Void>(0x77B21BE7AC540F07, e, x, y, z, w); }
	inline void ActivatePhysics(Entity e) { invoke<Void>(0x710311ADF0E20730, e); }
	inline void SetPedIntoVehicle(Ped p, Vehicle v, int seat) { invoke<Void>(0xF75B0D629E1C063D, p, v, seat); }
	inline void TaskLeaveVehicle(Ped p, Vehicle v, int flags) { invoke<Void>(0xD3DBCE61A490BE02, p, v, flags); }
	inline void TaskSmartFleePed(Ped p, Ped from) { invoke<Void>(0x22B0D0E37CCB840D, p, from, 100.0f, -1, FALSE, FALSE); }
	inline BOOL IsPedInVehicle(Ped p, Vehicle v) { return invoke<BOOL>(0xA3EE4A07279BB9DB, p, v, FALSE); }
	inline Ped GetPedInVehicleSeat(Vehicle v, int seat) { return invoke<Ped>(0xBB40DD2270B65366, v, seat, FALSE); }
	inline BOOL IsVehicleDriveable(Vehicle v) { return invoke<BOOL>(0x4C241E39B23DF959, v, FALSE); }
	inline Vehicle GetVehiclePedIsIn(Ped p, BOOL last) { return invoke<Vehicle>(0x9A9112A0FE9A4713, p, last); }
	inline BOOL GetIsDoorValid(Vehicle v, int door) { return invoke<BOOL>(0x645F4B6E8499F632, v, door); }
	inline void SetVehicleDoorOpen(Vehicle v, int door) { invoke<Void>(0x7C65DAC73C35C862, v, door, FALSE, FALSE); }
	inline void SetVehicleDoorShut(Vehicle v, int door) { invoke<Void>(0x93D9BD300D7789E5, v, door, FALSE); }
	inline void SetVehicleEngineOn(Vehicle v) { invoke<Void>(0x2497C4717C8B881E, v, TRUE, TRUE, FALSE); }
	inline int GetEntityBoneIndexByName(Entity e, const char *bone) { return invoke<int>(0xFB71170B7E76ACBA, e, bone); }
	inline Vector3 GetWorldPositionOfEntityBone(Entity e, int bone) { return invoke<Vector3>(0x44A8FCB8ED227738, e, bone); }
	inline Vector3 GetOffsetFromEntityInWorldCoords(Entity e, float x, float y, float z) { return invoke<Vector3>(0x1899F328B0E12848, e, x, y, z); }
	inline int GetPlayerWantedLevel(Player p) { return invoke<int>(0xE28E54788CE8F12D, p); }
	inline void SetPlayerWantedLevel(Player p, int level)
	{
		invoke<Void>(0x39FF19C64EF7DA5B, p, level, FALSE);
		invoke<Void>(0xE0A7D1E497FFCD6F, p, FALSE); // SET_PLAYER_WANTED_LEVEL_NOW
	}
	inline void AddOwnedExplosion(Ped owner, float x, float y, float z, int type, float damageScale, float cameraShake)
	{
		invoke<Void>(0x172AA1B624FA1013, owner, x, y, z, type, damageScale, TRUE, FALSE, cameraShake);
	}
	inline void TaskCombatPed(Ped p, Ped target) { invoke<Void>(0xF166E48407BAC484, p, target, 0, 16); }
	inline int GetEntityMaxHealth(Entity e) { return invoke<int>(0x15D757606D170C3C, e); }
	inline void SetEntityVelocity(Entity e, float x, float y, float z) { invoke<Void>(0x1C99BB7B6E96D16F, e, x, y, z); }
	inline Vector3 GetEntityVelocity(Entity e) { return invoke<Vector3>(0x4805D2B1D8CF94A9, e); }
	inline void SetMaxWantedLevel(int level) { invoke<Void>(0xAA5F02DB48D704B9, level); }
	inline void ClearPlayerWantedLevel(Player p) { invoke<Void>(0xB302540597885499, p); }

	inline Vector3 GetFinalRenderedCamCoord() { return invoke<Vector3>(0xA200EB1EE790F448); }
	inline Vector3 GetFinalRenderedCamRot(int order) { return invoke<Vector3>(0x5B4E4C817FCC2DFB, order); }
	inline float GetFinalRenderedCamFov() { return invoke<float>(0x80EC114669DAEFF4); }
	inline float GetFinalRenderedCamNearClip() { return invoke<float>(0xD0082607100D7193); }
	inline float GetFinalRenderedCamFarClip() { return invoke<float>(0xDFC8CBC606FDB0FC); }
	inline void GetActualScreenResolution(int *x, int *y) { invoke<Void>(0x873C9F3104101DD3, x, y); }
	inline Cam CreateCam(const char *name) { return invoke<Cam>(0xC3981DCE61D9E13F, name, TRUE); }
	inline void DestroyCam(Cam c) { invoke<Void>(0x865908C81A2C22E9, c, FALSE); }
	inline void SetCamCoord(Cam c, float x, float y, float z) { invoke<Void>(0x4D41783FB745E42E, c, x, y, z); }
	inline void SetCamRot(Cam c, float pitch, float roll, float yaw) { invoke<Void>(0x85973643155D0B07, c, pitch, roll, yaw, 2); }
	inline void SetCamFov(Cam c, float fov) { invoke<Void>(0xB13C14F66A00D047, c, fov); }
	inline void SetCamActive(Cam c, BOOL active) { invoke<Void>(0x026FB97D0A425F84, c, active); }
	inline void RenderScriptCams(BOOL render) { invoke<Void>(0x07E5B515DB0636FC, render, FALSE, 0, TRUE, FALSE, 0); }
	inline void ShakeCam(Cam c, const char *type, float amplitude) { invoke<Void>(0x6A25241C340D3822, c, type, amplitude); }
	inline void InvalidateIdleCam() { invoke<Void>(0xF4F2C0D4EE209E20); }

	inline BOOL GetGroundZFor3dCoord(float x, float y, float z, float *groundZ, BOOL ignoreWater, BOOL p5)
	{
		return invoke<BOOL>(0xC906A7DAB05C8D2B, x, y, z, groundZ, ignoreWater, p5);
	}
	inline void SetPedFootstepsEventsEnabled(Ped p, BOOL enabled) { invoke<Void>(0x0653B735BFBDFE87, p, enabled); }
	inline void AddExplosion(float x, float y, float z, int type, float damageScale, BOOL audible, BOOL invisible, float cameraShake, BOOL noDamage)
	{
		invoke<Void>(0xE3AD2BDBAEE269AC, x, y, z, type, damageScale, audible, invisible, cameraShake, noDamage);
	}

	inline void DisableAllControlActions(int group) { invoke<Void>(0x5F4B6931816E599B, group); }
	inline void EnableControlAction(int group, int control, BOOL enable) { invoke<Void>(0x351220255D64C155, group, control, enable); }
	inline float GetDisabledControlNormal(int group, int control) { return invoke<float>(0x11E65974A982637C, group, control); }
	inline BOOL IsDisabledControlJustPressed(int group, int control) { return invoke<BOOL>(0x91AEF906BCA88877, group, control); }
	inline void DrawRect(float x, float y, float width, float height, int r, int g, int b, int a) { invoke<Void>(0x3A618A217E5154F0, x, y, width, height, r, g, b, a, FALSE); }
	inline void DrawBox(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b, int a)
	{
		invoke<Void>(0xD3A9971CADAC7252, x1, y1, z1, x2, y2, z2, r, g, b, a);
	}
	inline void SetMouseCursorVisible(BOOL visible) { invoke<Void>(0x98215325A695E78A, visible); }
	inline void SetMouseCursorThisFrame() { invoke<Void>(0xAAE7CE1D63167423); }
	inline void HideHudAndRadarThisFrame() { invoke<Void>(0x719FF505F097FD20); }

	inline BOOL IsPauseMenuActive() { return invoke<BOOL>(0xB0034A223497FFCB); }
	inline BOOL IsScreenFadedOut() { return invoke<BOOL>(0xB16FCE9DDC7BA182); }
	inline BOOL IsCutsceneActive() { return invoke<BOOL>(0x991251AFC3981F84); }
	inline BOOL IsPlayerSwitchInProgress() { return invoke<BOOL>(0xD9D2CFFF49FAB35F); }
	inline int GetClockHours() { return invoke<int>(0x25223CA6B4D20B7F); }
	inline BOOL DoesEntityExist(Entity e) { return invoke<BOOL>(0x7239B21A38F536BA, e); }
	inline int GetEntityType(Entity e) { return invoke<int>(0x8ACD366038D14505, e); }
	inline Hash GetEntityModel(Entity e) { return invoke<Hash>(0x9F47B058362C84B5, e); }
	inline void GetEntityMatrix(Entity e, Vector3 *forward, Vector3 *right, Vector3 *up, Vector3 *position)
	{
		invoke<Void>(0xECB2FC7235A7D137, e, forward, right, up, position);
	}
	inline int GetEntityHealth(Entity e) { return invoke<int>(0xEEF059FAD016D209, e); }
	inline void SetEntityHealth(Entity e, int health, Entity instigator) { invoke<Void>(0x6B76DC1F3AE6E6A3, e, health, instigator, 0); }
	inline void SetStateOfClosestDoorOfType(Hash model, float x, float y, float z, BOOL locked, float open)
	{
		invoke<Void>(0xF82D8F1926A02C3D, model, x, y, z, locked, open, FALSE);
	}
	inline void SetPedMaxHealth(Ped p, int health) { invoke<Void>(0xF5F6378C4F3419D3, p, health); }
	inline void SetEntityProofs(Entity e, BOOL bullet, BOOL fire, BOOL explosion, BOOL collision, BOOL melee)
	{
		invoke<Void>(0xFAEE099C6F890BB8, e, bullet, fire, explosion, collision, melee, FALSE, FALSE, FALSE);
	}
	inline BOOL IsEntityDead(Entity e) { return invoke<BOOL>(0x5F9532F3B5CC2551, e, FALSE); }
	inline BOOL IsEntityVisible(Entity e) { return invoke<BOOL>(0x47D6F43D77935C75, e); }
	inline BOOL IsEntityAttached(Entity e) { return invoke<BOOL>(0xB346476EF1A64897, e); }
	inline BOOL GetEntityCollisionDisabled(Entity e) { return invoke<BOOL>(0xCCF1E97BEFDAE480, e); }
	inline BOOL IsPedInAnyVehicle(Ped p) { return invoke<BOOL>(0x997ABD671D25CA0B, p, FALSE); }
	inline BOOL IsPedRagdoll(Ped p) { return invoke<BOOL>(0x47E4E977581C5B55, p); }
	inline void SetPedToRagdoll(Ped p, int milliseconds) { invoke<BOOL>(0xAE99FB955581844A, p, milliseconds, milliseconds, 0, FALSE, FALSE, FALSE); }
	inline void ApplyForceToEntityCentreOfMass(Entity e, float x, float y, float z)
	{
		// An impulse along the world's axes, scaled by the entity's mass: a change of velocity
		invoke<Void>(0x18FF00FC7EFF559E, e, 1, x, y, z, FALSE, FALSE, TRUE, FALSE);
	}
	inline BOOL IsModelInCdimage(Hash model) { return invoke<BOOL>(0x35B9E0803292B641, model); }
	inline void GetModelDimensions(Hash model, Vector3 *minimum, Vector3 *maximum) { invoke<Void>(0x03E8D3D5F549087A, model, minimum, maximum); }
	inline void RequestModel(Hash model) { invoke<Void>(0x963D27A58DF860AC, model); }
	inline BOOL HasModelLoaded(Hash model) { return invoke<BOOL>(0x98A4EB5D89A0C952, model); }
	inline void SetModelAsNoLongerNeeded(Hash model) { invoke<Void>(0xE532F5D78798DAAB, model); }
	inline int GetClockSeconds() { return invoke<int>(0x494E97C2EF27C470); }
	inline int GetInteriorFromEntity(Entity e) { return invoke<int>(0x2107BA504071A6BB, e); }
	inline void GetCurrWeatherState(Hash *from, Hash *to, float *toShare) { invoke<Void>(0xF3BBE884A14BB413, from, to, toShare); }
	inline void RequestCollisionAtCoord(float x, float y, float z) { invoke<Void>(0x07503F7948F491A7, x, y, z); }
	inline void ClearArea(float x, float y, float z, float radius) { invoke<Void>(0xA56F01F3765B93A0, x, y, z, radius, TRUE, FALSE, FALSE, FALSE); }
	inline int GetClockMinutes() { return invoke<int>(0x13D2B8ADD79640F2); }
	inline void SetClockTime(int hour, int minute, int second) { invoke<Void>(0x47C3B5848C3E45D8, hour, minute, second); }
	inline void SetWeatherTypeNowPersist(const char *weather) { invoke<Void>(0xED712CA327900C8A, weather); }

	inline void Notify(const char *text)
	{
		invoke<Void>(0x202709F4C58A0424, "STRING");
		invoke<Void>(0x6C188BE134E074AA, text);
		invoke<int>(0x2ED7843F8F801023, FALSE, FALSE);
	}
}
