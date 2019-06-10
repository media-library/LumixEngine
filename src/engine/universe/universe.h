#pragma once


#include "engine/array.h"
#include "engine/delegate_list.h"
#include "engine/iplugin.h"
#include "engine/lumix.h"
#include "engine/matrix.h"
#include "engine/quat.h"
#include "engine/string.h"


namespace Lumix
{


struct ComponentUID;
struct IDeserializer;
class InputBlob;
struct IScene;
struct ISerializer;
class OutputBlob;
struct PrefabResource;


class LUMIX_ENGINE_API Universe
{
public:
	typedef void (IScene::*Create)(EntityRef);
	typedef void (IScene::*Destroy)(EntityRef);
	typedef void (IScene::*Serialize)(ISerializer&, EntityRef);
	typedef void (IScene::*Deserialize)(IDeserializer&, EntityRef, int);
	struct ComponentTypeEntry
	{
		IScene* scene = nullptr;
		void (IScene::*create)(EntityRef);
		void (IScene::*destroy)(EntityRef);
		void (IScene::*serialize)(ISerializer&, EntityRef);
		void (IScene::*deserialize)(IDeserializer&, EntityRef, int);
	};

	enum { ENTITY_NAME_MAX_LENGTH = 32 };

	struct EntityData
	{
		EntityData() {}

		int hierarchy;
		int name;

		union
		{
			struct 
			{
				u64 components;
			};
			struct
			{
				int prev;
				int next;
			};
		};
		bool valid;
	};

public:
	explicit Universe(IAllocator& allocator);
	~Universe();

	IAllocator& getAllocator() { return m_allocator; }
	const Transform* getTransforms() const { return m_transforms.begin(); }
	void emplaceEntity(EntityRef entity);
	EntityRef createEntity(const DVec3& position, const Quat& rotation);
	EntityRef cloneEntity(EntityRef entity);
	void destroyEntity(EntityRef entity);
	void createComponent(ComponentType type, EntityRef entity);
	void destroyComponent(EntityRef entity, ComponentType type);
	void onComponentCreated(EntityRef entity, ComponentType component_type, IScene* scene);
	void onComponentDestroyed(EntityRef entity, ComponentType component_type, IScene* scene);
    u64 getComponentsMask(EntityRef entity) const;
    bool hasComponent(EntityRef entity, ComponentType component_type) const;
	ComponentUID getComponent(EntityRef entity, ComponentType type) const;
	ComponentUID getFirstComponent(EntityRef entity) const;
	ComponentUID getNextComponent(const ComponentUID& cmp) const;
	ComponentTypeEntry& registerComponentType(ComponentType type) { return m_component_type_map[type.index]; }
	template <typename T1, typename T2, typename T3, typename T4>
	void registerComponentType(ComponentType type, IScene* scene, T1 create, T2 destroy, T3 serialize, T4 deserialize)
	{
		m_component_type_map[type.index].scene = scene;
		m_component_type_map[type.index].create = static_cast<Create>(create);
		m_component_type_map[type.index].destroy = static_cast<Destroy>(destroy);
		m_component_type_map[type.index].serialize = static_cast<Serialize>(serialize);
		m_component_type_map[type.index].deserialize = static_cast<Deserialize>(deserialize);
	}

	EntityPtr getFirstEntity() const;
	EntityPtr getNextEntity(EntityRef entity) const;
	const char* getEntityName(EntityRef entity) const;
	EntityPtr findByName(EntityPtr parent, const char* name);
	void setEntityName(EntityRef entity, const char* name);
	bool hasEntity(EntityRef entity) const;

	bool isDescendant(EntityRef ancestor, EntityRef descendant) const;
	EntityPtr getParent(EntityRef entity) const;
	EntityPtr getFirstChild(EntityRef entity) const;
	EntityPtr getNextSibling(EntityRef entity) const;
	Transform getLocalTransform(EntityRef entity) const;
	float getLocalScale(EntityRef entity) const;
	void setParent(EntityPtr parent, EntityRef child);
	void setLocalPosition(EntityRef entity, const DVec3& pos);
	void setLocalRotation(EntityRef entity, const Quat& rot);
	void setLocalTransform(EntityRef entity, const Transform& transform);
	Transform computeLocalTransform(EntityRef parent, const Transform& global_transform) const;

	Matrix getRelativeMatrix(EntityRef entity, const DVec3& base_pos) const;
	void setTransform(EntityRef entity, const RigidTransform& transform);
	void setTransform(EntityRef entity, const Transform& transform);
	void setTransformKeepChildren(EntityRef entity, const Transform& transform);
	void setTransform(EntityRef entity, const DVec3& pos, const Quat& rot, float scale);
	const Transform& getTransform(EntityRef entity) const;
	void setRotation(EntityRef entity, float x, float y, float z, float w);
	void setRotation(EntityRef entity, const Quat& rot);
	void setPosition(EntityRef entity, double x, double y, double z);
	void setPosition(EntityRef entity, const DVec3& pos);
	void setScale(EntityRef entity, float scale);
	EntityPtr instantiatePrefab(const PrefabResource& prefab,
		const DVec3& pos,
		const Quat& rot,
		float scale);
	float getScale(EntityRef entity) const;
	const DVec3& getPosition(EntityRef entity) const;
	const Quat& getRotation(EntityRef entity) const;
	const char* getName() const { return m_name; }
	void setName(const char* name) 
	{ 
		m_name = name; 
	}

	DelegateList<void(EntityRef)>& entityTransformed() { return m_entity_moved; }
	DelegateList<void(EntityRef)>& entityCreated() { return m_entity_created; }
	DelegateList<void(EntityRef)>& entityDestroyed() { return m_entity_destroyed; }
	DelegateList<void(const ComponentUID&)>& componentDestroyed() { return m_component_destroyed; }
	DelegateList<void(const ComponentUID&)>& componentAdded() { return m_component_added; }

	void serializeComponent(ISerializer& serializer, ComponentType type, EntityRef entity);
	void deserializeComponent(IDeserializer& serializer, EntityRef entity, ComponentType type, int scene_version);
	void serialize(OutputBlob& serializer);
	void deserialize(InputBlob& serializer);

	IScene* getScene(ComponentType type) const;
	IScene* getScene(u32 hash) const;
	Array<IScene*>& getScenes();
	void addScene(IScene* scene);
	void removeScene(IScene* scene);

private:
	void transformEntity(EntityRef entity, bool update_local);
	void updateGlobalTransform(EntityRef entity);

	struct Hierarchy
	{
		EntityRef entity;
		EntityPtr parent;
		EntityPtr first_child;
		EntityPtr next_sibling;

		Transform local_transform;
	};

	struct EntityName
	{
		EntityRef entity;
		char name[ENTITY_NAME_MAX_LENGTH];
	};

private:
	IAllocator& m_allocator;
	ComponentTypeEntry m_component_type_map[ComponentType::MAX_TYPES_COUNT];
	Array<IScene*> m_scenes;
	Array<Transform> m_transforms;
	Array<EntityData> m_entities;
	Array<Hierarchy> m_hierarchy;
	Array<EntityName> m_names;
	DelegateList<void(EntityRef)> m_entity_moved;
	DelegateList<void(EntityRef)> m_entity_created;
	DelegateList<void(EntityRef)> m_entity_destroyed;
	DelegateList<void(const ComponentUID&)> m_component_destroyed;
	DelegateList<void(const ComponentUID&)> m_component_added;
	int m_first_free_slot;
	StaticString<64> m_name;
};


} // namespace Lumix
