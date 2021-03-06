#pragma once

#include "engine/lumix.h"

struct lua_State;

namespace Lumix
{

namespace OS {
	using WindowHandle = void*;
}

struct IAllocator;
class InputMemoryStream;
class OutputMemoryStream;
class Path;
class Universe;

class LUMIX_ENGINE_API Engine
{
public:
	struct InitArgs {
		const char* working_dir = nullptr;
		Span<const char*> plugins;
		bool fullscreen = false;
		bool handle_file_drops = false;
		const char* window_title = "Lumix App";
	};

	using LuaResourceHandle = u32;

public:
	virtual ~Engine() {}

	static Engine* create(const InitArgs& init_data, IAllocator& allocator);
	static void destroy(Engine* engine, IAllocator& allocator);

	virtual Universe& createUniverse(bool is_main_universe) = 0;
	virtual void destroyUniverse(Universe& context) = 0;
	virtual OS::WindowHandle getWindowHandle() = 0;

	virtual struct PathManager& getPathManager() = 0;
	virtual class FileSystem& getFileSystem() = 0;
	virtual class InputSystem& getInputSystem() = 0;
	virtual class PluginManager& getPluginManager() = 0;
	virtual class ResourceManagerHub& getResourceManager() = 0;
	virtual class PageAllocator& getPageAllocator() = 0;
	virtual IAllocator& getAllocator() = 0;
	virtual bool instantiatePrefab(Universe& universe,
		const struct PrefabResource& prefab,
		const struct DVec3& pos,
		const struct Quat& rot,
		float scale,
		Ref<struct EntityMap> entity_map) = 0;

	virtual void startGame(Universe& context) = 0;
	virtual void stopGame(Universe& context) = 0;

	virtual void update(Universe& context) = 0;
	virtual u32 serialize(Universe& ctx, OutputMemoryStream& serializer) = 0;
	virtual bool deserialize(Universe& ctx, InputMemoryStream& serializer, Ref<struct EntityMap> entity_map) = 0;
	virtual float getLastTimeDelta() const = 0;
	virtual void setTimeMultiplier(float multiplier) = 0;
	virtual void pause(bool pause) = 0;
	virtual void nextFrame() = 0;
	virtual lua_State* getState() = 0;

	virtual class Resource* getLuaResource(LuaResourceHandle idx) const = 0;
	virtual LuaResourceHandle addLuaResource(const Path& path, struct ResourceType type) = 0;
	virtual void unloadLuaResource(LuaResourceHandle resource_idx) = 0;

protected:
	Engine() {}
};


} // namespace Lumix
