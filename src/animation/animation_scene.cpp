#include "animation_scene.h"

#include "animation/animation.h"
#include "animation/controller.h"
#include "animation/events.h"
#include "animation/property_animation.h"
#include "engine/associative_array.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/lua_wrapper.h"
#include "engine/mt/atomic.h"
#include "engine/job_system.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/universe/universe.h"
#include "nodes.h"
#include "renderer/model.h"
#include "renderer/pose.h"
#include "renderer/render_scene.h"
#include "string.h" // memcpy


namespace Lumix
{

class Animation;
class Engine;
class Universe;

enum class AnimationSceneVersion
{
	FIRST,

	LATEST
};


static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");
static const ComponentType ANIMABLE_TYPE = Reflection::getComponentType("animable");
static const ComponentType PROPERTY_ANIMATOR_TYPE = Reflection::getComponentType("property_animator");
static const ComponentType ANIMATOR_TYPE = Reflection::getComponentType("animator");


struct AnimationSceneImpl final : public AnimationScene
{
	friend struct AnimationSystemImpl;
	
	struct Animator
	{
		EntityRef entity;
		Anim::Controller* resource = nullptr;
		u32 default_set = 0;
		Anim::RuntimeContext* ctx = nullptr;

		struct IK {
			float weight = 0;
			Vec3 target;
		} inverse_kinematics[4];
	};


	struct PropertyAnimator
	{
		struct Key
		{
			int frame0;
			int frame1;
			float value0;
			float value1;
		};

		enum Flags
		{
			LOOPED = 1 << 0,
			DISABLED = 1 << 1
		};

		PropertyAnimator(IAllocator& allocator) : keys(allocator) {}

		PropertyAnimation* animation;
		Array<Key> keys;

		FlagSet<Flags, u32> flags;
		float time;
	};


	AnimationSceneImpl(Engine& engine, IPlugin& anim_system, Universe& universe, IAllocator& allocator)
		: m_universe(universe)
		, m_engine(engine)
		, m_anim_system(anim_system)
		, m_animables(allocator)
		, m_property_animators(allocator)
		, m_animators(allocator)
		, m_event_stream(allocator)
		, m_allocator(allocator)
		, m_animator_map(allocator)
	{
		m_is_game_running = false;
		m_render_scene = static_cast<RenderScene*>(universe.getScene(crc32("renderer")));
		universe.registerComponentType(PROPERTY_ANIMATOR_TYPE
			, this
			, &AnimationSceneImpl::createPropertyAnimator
			, &AnimationSceneImpl::destroyPropertyAnimator);
		universe.registerComponentType(ANIMABLE_TYPE
			, this
			, &AnimationSceneImpl::createAnimable
			, &AnimationSceneImpl::destroyAnimable);
		universe.registerComponentType(ANIMATOR_TYPE
			, this
			, &AnimationSceneImpl::createAnimator
			, &AnimationSceneImpl::destroyAnimator);
		ASSERT(m_render_scene);
	}


	int getVersion() const override { return (int)AnimationSceneVersion::LATEST; }


	const OutputMemoryStream& getEventStream() const override
	{
		return m_event_stream;
	}


	void clear() override
	{
		for (PropertyAnimator& anim : m_property_animators)
		{
			unloadResource(anim.animation);
		}
		m_property_animators.clear();

		for (Animable& animable : m_animables)
		{
			unloadResource(animable.animation);
		}
		m_animables.clear();

		for (Animator& animator : m_animators)
		{
			unloadResource(animator.resource);
			setAnimatorSource(animator, nullptr);
		}
		m_animators.clear();
	}


	static int setIK(lua_State* L)
	{
		AnimationSceneImpl* scene = LuaWrapper::checkArg<AnimationSceneImpl*>(L, 1);
		EntityRef entity = LuaWrapper::checkArg<EntityRef>(L, 2);
		auto iter = scene->m_animator_map.find(entity);
		if (!iter.isValid()) {
			luaL_argerror(L, 2, "entity does not have animator");
		}
		Animator& animator = scene->m_animators[iter.value()];
		const u32 index = LuaWrapper::checkArg<u32>(L, 3);
		if (index >= lengthOf(animator.inverse_kinematics)) {
			luaL_argerror(L, 3, "Inverse kinematics index out of range");
		}
		Animator::IK& ik = animator.inverse_kinematics[index];
		ik.weight = clamp(LuaWrapper::checkArg<float>(L, 4), 0.f, 1.f);
		ik.target = LuaWrapper::checkArg<Vec3>(L, 5);

		return 0;
	}


	int getAnimatorInputIndex(EntityRef entity, const char* name) const override
	{
		const Animator& animator = m_animators[m_animator_map[entity]];
		Anim::InputDecl& decl = animator.resource->m_inputs;
		for (u32 i = 0; i < lengthOf(decl.inputs); ++i) {
			if (decl.inputs[i].type != Anim::InputDecl::EMPTY && equalStrings(decl.inputs[i].name, name)) return i;
		}
		return -1;
	}


	void setAnimatorFloatInput(EntityRef entity, u32 input_idx, float value)
	{
		auto iter = m_animator_map.find(entity);
		if (!iter.isValid()) return;

		Animator& animator = m_animators[iter.value()];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		if (input_idx >= decl.inputs_count) return;
		if (!animator.ctx) return;

		if (decl.inputs[input_idx].type == Anim::InputDecl::FLOAT) {
			memcpy(&animator.ctx->inputs[decl.inputs[input_idx].offset], &value, sizeof(value));
		}
		else {
			logWarning("Animation") << "Trying to set float to " << decl.inputs[input_idx].name;
		}
	}


	void setAnimatorU32Input(EntityRef entity, u32 input_idx, u32 value)
	{
		auto iter = m_animator_map.find(entity);
		if (!iter.isValid()) return;

		Animator& animator = m_animators[iter.value()];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		if (input_idx >= decl.inputs_count) return;
		if (!animator.ctx) return;

		if (decl.inputs[input_idx].type == Anim::InputDecl::U32) {
			*(u32*)&animator.ctx->inputs[decl.inputs[input_idx].offset] = value;
		}
		else {
			logWarning("Animation") << "Trying to set int to " << decl.inputs[input_idx].name;
		}
	}


	void setAnimatorBoolInput(EntityRef entity, u32 input_idx, bool value)
	{
		auto iter = m_animator_map.find(entity);
		if (!iter.isValid()) return;

		Animator& animator = m_animators[iter.value()];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		if (input_idx >= decl.inputs_count) return;
		if (!animator.ctx) return;

		if (decl.inputs[input_idx].type == Anim::InputDecl::BOOL) {
			*(bool*)&animator.ctx->inputs[decl.inputs[input_idx].offset] = value;
		}
		else {
			logWarning("Animation") << "Trying to set bool to " << decl.inputs[input_idx].name;
		}
	}


	float getAnimationLength(int animation_idx) override
	{
		auto* animation = static_cast<Animation*>(animation_idx > 0 ? m_engine.getLuaResource(animation_idx) : nullptr);
		if (animation) return animation->getLength().seconds();
		return 0;
	}


	Animable& getAnimable(EntityRef entity) override
	{
		return m_animables[entity];
	}


	Animation* getAnimableAnimation(EntityRef entity) override
	{
		return m_animables[entity].animation;
	}

	
	void startGame() override 
	{
		m_is_game_running = true;
	}
	
	
	void stopGame() override
	{
		m_is_game_running = false;
	}
	
	
	Universe& getUniverse() override { return m_universe; }


	static void unloadResource(Resource* res)
	{
		if (!res) return;

		res->getResourceManager().unload(*res);
	}


	void setAnimatorSource(Animator& animator, Anim::Controller* res)
	{
		if (animator.resource == res) return;
		if (animator.resource != nullptr) {
			if (animator.ctx) {
				animator.resource->destroyRuntime(*animator.ctx);
				animator.ctx = nullptr;
			}
			animator.resource->getObserverCb().unbind<&AnimationSceneImpl::onControllerResourceChanged>(this);
		}
		animator.resource = res;
		if (animator.resource != nullptr) {
			animator.resource->onLoaded<&AnimationSceneImpl::onControllerResourceChanged>(this);
		}
	}


	void onControllerResourceChanged(Resource::State old_state, Resource::State new_state, Resource& resource)
	{
		for (Animator& animator : m_animators) {
			if (animator.resource == &resource) {
				if(new_state == Resource::State::READY) {
					ASSERT(!animator.ctx || old_state == Resource::State::READY);
					if (!animator.ctx) {
						animator.ctx = animator.resource->createRuntime(animator.default_set);
					}
				}
				else {
					if (animator.ctx) {
						animator.resource->destroyRuntime(*animator.ctx);
						animator.ctx = nullptr;
					}
				}
			}
		}
	}


	void destroyPropertyAnimator(EntityRef entity)
	{
		int idx = m_property_animators.find(entity);
		auto& animator = m_property_animators.at(idx);
		unloadResource(animator.animation);
		m_property_animators.erase(entity);
		m_universe.onComponentDestroyed(entity, PROPERTY_ANIMATOR_TYPE, this);
	}


	void destroyAnimable(EntityRef entity)
	{
		auto& animable = m_animables[entity];
		unloadResource(animable.animation);
		m_animables.erase(entity);
		m_universe.onComponentDestroyed(entity, ANIMABLE_TYPE, this);
	}


	void destroyAnimator(EntityRef entity)
	{
		const u32 idx = m_animator_map[entity];
		Animator& animator = m_animators[idx];
		unloadResource(animator.resource);
		setAnimatorSource(animator, nullptr);
		const Animator& last = m_animators.back();
		m_animator_map[last.entity] = idx;
		m_animator_map.erase(entity);
		m_animators.swapAndPop(idx);
		m_universe.onComponentDestroyed(entity, ANIMATOR_TYPE, this);
	}


	void serialize(OutputMemoryStream& serializer) override
	{
		serializer.write((u32)m_animables.size());
		for (const Animable& animable : m_animables)
		{
			serializer.write(animable.entity);
			serializer.writeString(animable.animation ? animable.animation->getPath().c_str() : "");
		}

		serializer.write((u32)m_property_animators.size());
		for (int i = 0, n = m_property_animators.size(); i < n; ++i)
		{
			const PropertyAnimator& animator = m_property_animators.at(i);
			EntityRef entity = m_property_animators.getKey(i);
			serializer.write(entity);
			serializer.writeString(animator.animation ? animator.animation->getPath().c_str() : "");
			serializer.write(animator.flags.base);
		}

		serializer.write((u32)m_animators.size());
		for (const Animator& animator : m_animators)
		{
			serializer.write(animator.default_set);
			serializer.write(animator.entity);
			serializer.writeString(animator.resource ? animator.resource->getPath().c_str() : "");
		}
	}


	void deserialize(InputMemoryStream& serializer, const EntityMap& entity_map) override
	{
		u32 count;
		serializer.read(count);
		m_animables.reserve(count + m_animables.size());
		for (u32 i = 0; i < count; ++i)
		{
			Animable animable;
			serializer.read(animable.entity);
			animable.entity = entity_map.get(animable.entity);
			animable.time = Time::fromSeconds(0);

			char path[MAX_PATH_LENGTH];
			serializer.readString(Span(path));
			animable.animation = path[0] == '\0' ? nullptr : loadAnimation(Path(path));
			m_animables.insert(animable.entity, animable);
			m_universe.onComponentCreated(animable.entity, ANIMABLE_TYPE, this);
		}

		serializer.read(count);
		m_property_animators.reserve(count + m_property_animators.size());
		for (u32 i = 0; i < count; ++i)
		{
			EntityRef entity;
			serializer.read(entity);
			entity = entity_map.get(entity);

			PropertyAnimator& animator = m_property_animators.emplace(entity, m_allocator);
			char path[MAX_PATH_LENGTH];
			serializer.readString(Span(path));
			serializer.read(animator.flags.base);
			animator.time = 0;
			animator.animation = loadPropertyAnimation(Path(path));
			m_universe.onComponentCreated(entity, PROPERTY_ANIMATOR_TYPE, this);
		}


		serializer.read(count);
		m_animators.reserve(m_animators.size() + count);
		for (u32 i = 0; i < count; ++i)
		{
			Animator animator;
			serializer.read(animator.default_set);
			serializer.read(animator.entity);
			animator.entity = entity_map.get(animator.entity);

			char tmp[MAX_PATH_LENGTH];
			serializer.readString(Span(tmp));
			setAnimatorSource(animator, tmp[0] ? loadController(Path(tmp)) : nullptr);
			m_animator_map.insert(animator.entity, m_animators.size());
			m_animators.emplace(static_cast<Animator&&>(animator));
			m_universe.onComponentCreated(animator.entity, ANIMATOR_TYPE, this);
		}
	}


	void setAnimatorSource(EntityRef entity, const Path& path) override
	{
		Animator& animator = m_animators[m_animator_map[entity]];
		unloadResource(animator.resource);
		setAnimatorSource(animator, path.isValid() ? loadController(path) : nullptr);
		if (animator.resource && animator.resource->isReady() && m_is_game_running) {
			ASSERT(false);
			// TODO
		}
	}


	Path getAnimatorSource(EntityRef entity) override
	{
		const Animator& animator = m_animators[m_animator_map[entity]];
		return animator.resource ? animator.resource->getPath() : Path("");
	}

	bool isPropertyAnimatorEnabled(EntityRef entity) override
	{
		return !m_property_animators.get(entity).flags.isSet(PropertyAnimator::DISABLED);
	}


	void enablePropertyAnimator(EntityRef entity, bool enabled) override
	{
		PropertyAnimator& animator = m_property_animators.get(entity);
		animator.flags.set(PropertyAnimator::DISABLED, !enabled);
		animator.time = 0;
		if (!enabled)
		{
			applyPropertyAnimator(entity, animator);
		}
	}


	Path getPropertyAnimation(EntityRef entity) override
	{
		const auto& animator = m_property_animators.get(entity);
		return animator.animation ? animator.animation->getPath() : Path("");
	}
	
	
	void setPropertyAnimation(EntityRef entity, const Path& path) override
	{
		auto& animator = m_property_animators.get(entity);
		animator.time = 0;
		unloadResource(animator.animation);
		animator.animation = loadPropertyAnimation(path);
	}


	Path getAnimation(EntityRef entity) override
	{
		const auto& animable = m_animables[entity];
		return animable.animation ? animable.animation->getPath() : Path("");
	}


	void setAnimation(EntityRef entity, const Path& path) override
	{
		auto& animable = m_animables[entity];
		unloadResource(animable.animation);
		animable.animation = loadAnimation(path);
		animable.time = Time::fromSeconds(0);
	}


	void updateAnimable(Animable& animable, float time_delta) const
	{
		if (!animable.animation || !animable.animation->isReady()) return;
		EntityRef entity = animable.entity;
		if (!m_universe.hasComponent(entity, MODEL_INSTANCE_TYPE)) return;

		Model* model = m_render_scene->getModelInstanceModel(entity);
		if (!model->isReady()) return;

		Pose* pose = m_render_scene->lockPose(entity);
		if (!pose) return;

		model->getRelativePose(*pose);
		animable.animation->getRelativePose(animable.time, *pose, *model, nullptr);
		pose->computeAbsolute(*model);

		Time t = animable.time + Time::fromSeconds(time_delta);
		const Time l = animable.animation->getLength();
		t = t % l;
		animable.time = t;

		m_render_scene->unlockPose(entity, true);
	}


	void updateAnimable(EntityRef entity, float time_delta) override
	{
		Animable& animable = m_animables[entity];
		updateAnimable(animable, time_delta);
	}


	void updateAnimator(EntityRef entity, float time_delta) override {
		Animator& animator = m_animators[m_animator_map[entity]];
		updateAnimator(animator, time_delta);
		processEventStream();
		m_event_stream.clear();
	}

	void setAnimatorInput(EntityRef entity, u32 input_idx, float value) override {
		Animator& animator = m_animators[m_animator_map[entity]];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		ASSERT(input_idx >= lengthOf(decl.inputs));
		ASSERT(decl.inputs[input_idx].type != Anim::InputDecl::FLOAT);

		*(float*)&animator.ctx->inputs[decl.inputs[input_idx].offset] = value;
	}

	void setAnimatorInput(EntityRef entity, u32 input_idx, bool value) override {
		Animator& animator = m_animators[m_animator_map[entity]];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		ASSERT(input_idx >= lengthOf(decl.inputs));
		ASSERT(decl.inputs[input_idx].type != Anim::InputDecl::BOOL);

		*(bool*)&animator.ctx->inputs[decl.inputs[input_idx].offset] = value;
	}

	void setAnimatorInput(EntityRef entity, u32 input_idx, u32 value) override {
		Animator& animator = m_animators[m_animator_map[entity]];
		const Anim::InputDecl& decl = animator.resource->m_inputs;
		ASSERT(input_idx >= lengthOf(decl.inputs));
		ASSERT(decl.inputs[input_idx].type != Anim::InputDecl::U32);

		*(u32*)&animator.ctx->inputs[decl.inputs[input_idx].offset] = value;
	}

	LocalRigidTransform getAnimatorRootMotion(EntityRef entity) override
	{
		ASSERT(false);
		// TODO
		return {};
	}


	void applyAnimatorSet(EntityRef entity, const char* set_name) override
	{
		/*Animator& ctrl = m_animators[entity];
		const u32 set_name_hash = crc32(set_name);
		int set_idx = ctrl.resource->m_sets_names.find([set_name_hash](const StaticString<32>& val) {
			return crc32(val) == set_name_hash;
		});
		if (set_idx < 0) return;

		for (auto& entry : ctrl.resource->m_animation_set)
		{
			if (entry.set != set_idx) continue;
			ctrl.animations[entry.hash] = entry.animation;
		}
		ASSERT(false);
		// TODO
		//if (ctrl.root) ctrl.root->onAnimationSetUpdated(ctrl.animations);*/
	}


	void setAnimatorDefaultSet(EntityRef entity, u32 set) override
	{
		// TODO
		ASSERT(false);
		/*
		Animator& ctrl = m_animators.get(entity);
		ctrl.default_set = ctrl.resource ? crc32(ctrl.resource->m_sets_names[set]) : 0;*/
	}


	int getAnimatorDefaultSet(EntityRef entity) override
	{
		// TODO
		ASSERT(false);
		/*
		Animator& ctrl = m_animators.get(entity);
		auto is_default_set = [&ctrl](const StaticString<32>& val) {
			return crc32(val) == ctrl.default_set;
		};
		int idx = 0;
		if(ctrl.resource) idx = ctrl.resource->m_sets_names.find(is_default_set);
		return idx < 0 ? 0 : idx;*/
		return -1;
	}

	void updateAnimator(Animator& animator, float time_delta)
	{
		if (!animator.resource || !animator.resource->isReady()) return;
		if (!animator.ctx) {
			animator.ctx = animator.resource->createRuntime(animator.default_set);
		}

		const EntityRef entity = animator.entity;
		if (!m_universe.hasComponent(entity, MODEL_INSTANCE_TYPE)) return;

		Model* model = m_render_scene->getModelInstanceModel(entity);
		if (!model->isReady()) return;

		Pose* pose = m_render_scene->lockPose(entity);
		if (!pose) return;

		animator.ctx->model = model;
		animator.ctx->time_delta = Time::fromSeconds(time_delta);
		// TODO
		animator.ctx->root_bone_hash = crc32("RigRoot");
		LocalRigidTransform root_motion;
		animator.resource->update(*animator.ctx, Ref(root_motion));

		if (animator.resource->m_flags.isSet(Anim::Controller::Flags::USE_ROOT_MOTION)) {
			Transform tr = m_universe.getTransform(entity);
			tr.rot = tr.rot * root_motion.rot; 
			tr.pos = tr.pos + tr.rot.rotate(root_motion.pos);
			m_universe.setTransform(entity, tr);
		}

		model->getRelativePose(*pose);
		animator.resource->getPose(*animator.ctx, Ref(*pose));
		
		for (Animator::IK& ik : animator.inverse_kinematics) {
			if (ik.weight == 0) break;
			const u32 idx = u32(&ik - animator.inverse_kinematics);
			updateIK(animator.resource->m_ik[idx], ik, *pose, *model);
		}

		pose->computeAbsolute(*model);

		m_render_scene->unlockPose(entity, true);
	}

	static LocalRigidTransform getAbsolutePosition(const Pose& pose, const Model& model, int bone_index)
	{
		const Model::Bone& bone = model.getBone(bone_index);
		LocalRigidTransform bone_transform{pose.positions[bone_index], pose.rotations[bone_index]};
		if (bone.parent_idx < 0)
		{
			return bone_transform;
		}
		return getAbsolutePosition(pose, model, bone.parent_idx) * bone_transform;
	}

	static void updateIK(Anim::Controller::IK& res_ik, Animator::IK& ik, Pose& pose, Model& model)
	{
		u32 indices[Anim::Controller::IK::MAX_BONES_COUNT];
		LocalRigidTransform transforms[Anim::Controller::IK::MAX_BONES_COUNT];
		Vec3 old_pos[Anim::Controller::IK::MAX_BONES_COUNT];
		float len[Anim::Controller::IK::MAX_BONES_COUNT - 1];
		float len_sum = 0;
		for (int i = 0; i < res_ik.bones_count; ++i) {
			auto iter = model.getBoneIndex(res_ik.bones[i]);
			if (!iter.isValid()) return;

			indices[i] = iter.value();
		}

		// convert from bone space to object space
		const Model::Bone& first_bone = model.getBone(indices[0]);
		LocalRigidTransform roots_parent;
		if (first_bone.parent_idx >= 0) {
			roots_parent = getAbsolutePosition(pose, model, first_bone.parent_idx);
		}
		else {
			roots_parent.pos = Vec3::ZERO;
			roots_parent.rot = Quat::IDENTITY;
		}

		LocalRigidTransform parent_tr = roots_parent;
		for (int i = 0; i < res_ik.bones_count; ++i) {
			LocalRigidTransform tr{pose.positions[indices[i]], pose.rotations[indices[i]]};
			transforms[i] = parent_tr * tr;
			old_pos[i] = transforms[i].pos;
			if (i > 0) {
				len[i - 1] = (transforms[i].pos - transforms[i - 1].pos).length();
				len_sum += len[i - 1];
			}
			parent_tr = transforms[i];
		}

		Vec3 target = ik.target;
		Vec3 to_target = target - transforms[0].pos;
		if (len_sum * len_sum < to_target.squaredLength()) {
			to_target.normalize();
			target = transforms[0].pos + to_target * len_sum;
		}

		for (int iteration = 0; iteration < res_ik.max_iterations; ++iteration) {
			transforms[res_ik.bones_count - 1].pos = target;
			
			for (int i = res_ik.bones_count - 1; i > 1; --i) {
				Vec3 dir = (transforms[i - 1].pos - transforms[i].pos).normalized();
				transforms[i - 1].pos = transforms[i].pos + dir * len[i - 1];
			}

			for (int i = 1; i < res_ik.bones_count; ++i) {
				Vec3 dir = (transforms[i].pos - transforms[i - 1].pos).normalized();
				transforms[i].pos = transforms[i - 1].pos + dir * len[i - 1];
			}
		}

		// compute rotations from new positions
		for (int i = res_ik.bones_count - 2; i >= 0; --i) {
			Vec3 old_d = old_pos[i + 1] - old_pos[i];
			Vec3 new_d = transforms[i + 1].pos - transforms[i].pos;

			Quat rel_rot = Quat::vec3ToVec3(old_d, new_d);
			transforms[i].rot = rel_rot * transforms[i].rot;
		}

		// convert from object space to bone space
		LocalRigidTransform ik_out[Anim::Controller::IK::MAX_BONES_COUNT];
		for (int i = res_ik.bones_count - 1; i > 0; --i) {
			transforms[i] = transforms[i - 1].inverted() * transforms[i];
			ik_out[i].pos = transforms[i].pos;
		}
		for (int i = res_ik.bones_count - 2; i > 0; --i) {
			ik_out[i].rot = transforms[i].rot;
		}
		ik_out[res_ik.bones_count - 1].rot = pose.rotations[indices[res_ik.bones_count - 1]];

		if (first_bone.parent_idx >= 0) {
			ik_out[0].rot = roots_parent.rot.conjugated() * transforms[0].rot;
		}
		else {
			ik_out[0].rot = transforms[0].rot;
		}
		ik_out[0].pos = pose.positions[indices[0]];

		const float w = ik.weight;
		for (u32 i = 0; i < res_ik.bones_count; ++i) {
			const u32 idx = indices[i];
			pose.positions[idx] = lerp(pose.positions[idx], ik_out[i].pos, w);
			pose.rotations[idx] = nlerp(pose.rotations[idx], ik_out[i].rot, w);
		}
	}


	void applyPropertyAnimator(EntityRef entity, PropertyAnimator& animator)
	{
		const PropertyAnimation* animation = animator.animation;
		int frame = int(animator.time * animation->fps + 0.5f);
		frame = frame % animation->curves[0].frames.back();
		for (PropertyAnimation::Curve& curve : animation->curves)
		{
			if (curve.frames.size() < 2) continue;
			for (int i = 1, n = curve.frames.size(); i < n; ++i)
			{
				if (frame <= curve.frames[i])
				{
					float t = (frame - curve.frames[i - 1]) / float(curve.frames[i] - curve.frames[i - 1]);
					float v = curve.values[i] * t + curve.values[i - 1] * (1 - t);
					ComponentUID cmp;
					cmp.type = curve.cmp_type;
					cmp.scene = m_universe.getScene(cmp.type);
					cmp.entity = entity;
					InputMemoryStream blob(&v, sizeof(v));
					curve.property->setValue(cmp, -1, blob);
					break;
				}
			}
		}
	}


	void updatePropertyAnimators(float time_delta)
	{
		PROFILE_FUNCTION();
		for (int anim_idx = 0, c = m_property_animators.size(); anim_idx < c; ++anim_idx)
		{
			EntityRef entity = m_property_animators.getKey(anim_idx);
			PropertyAnimator& animator = m_property_animators.at(anim_idx);
			const PropertyAnimation* animation = animator.animation;
			if (!animation || !animation->isReady()) continue;
			if (animation->curves.empty()) continue;
			if (animation->curves[0].frames.empty()) continue;
			if (animator.flags.isSet(PropertyAnimator::DISABLED)) continue;

			animator.time += time_delta;
			
			applyPropertyAnimator(entity, animator);
		}
	}


	void updateAnimables(float time_delta)
	{
		PROFILE_FUNCTION();
		if (m_animables.size() == 0) return;

		JobSystem::forEach(m_animables.size(), [&](int idx){
			Animable& animable = m_animables.at(idx);
			updateAnimable(animable, time_delta);
		});
	}


	void update(float time_delta, bool paused) override
	{
		PROFILE_FUNCTION();
		if (!m_is_game_running) return;
		if (paused) return;

		m_event_stream.clear();

		updateAnimables(time_delta);
		updatePropertyAnimators(time_delta);

		i32 animator_idx = 0;
		JobSystem::runOnWorkers([&](){
			PROFILE_BLOCK("update animators");
			for(;;) {
				const i32 idx = MT::atomicIncrement(&animator_idx) - 1;
				if (idx >= (i32)m_animators.size()) return;
				updateAnimator(m_animators[idx], time_delta);
			}
		});

		processEventStream();
	}


	void processEventStream()
	{
		InputMemoryStream blob(m_event_stream);
		u32 set_input_type = crc32("set_input");
		while (blob.getPosition() < blob.size())
		{
			u32 type;
			u8 size;
			EntityRef entity;
			blob.read(type);
			blob.read(entity);
			blob.read(size);
			if (type == set_input_type)
			{
				Anim::SetInputEvent event;
				blob.read(event);
				Animator& ctrl = m_animators[m_animator_map[entity]];
				if (ctrl.resource->isReady())
				{
					Anim::InputDecl& decl = ctrl.resource->m_inputs;
					Anim::InputDecl::Input& input = decl.inputs[event.input_idx];
					switch (input.type)
					{
						case Anim::InputDecl::BOOL: *(bool*)&ctrl.ctx->inputs[input.offset] = event.b_value; break;
						case Anim::InputDecl::U32: *(u32*)&ctrl.ctx->inputs[input.offset] = event.i_value; break;
						case Anim::InputDecl::FLOAT: *(float*)&ctrl.ctx->inputs[input.offset] = event.f_value; break;
						default: ASSERT(false); break;
					}
				}
			}
			else
			{
				blob.skip(size);
			}
		}
	}


	PropertyAnimation* loadPropertyAnimation(const Path& path) const
	{
		if (!path.isValid()) return nullptr;
		ResourceManagerHub& rm = m_engine.getResourceManager();
		return rm.load<PropertyAnimation>(path);
	}


	Animation* loadAnimation(const Path& path) const
	{
		ResourceManagerHub& rm = m_engine.getResourceManager();
		return rm.load<Animation>(path);
	}


	Anim::Controller* loadController(const Path& path) const
	{
		ResourceManagerHub& rm = m_engine.getResourceManager();
		return rm.load<Anim::Controller>(path);
	}


	void createPropertyAnimator(EntityRef entity)
	{
		PropertyAnimator& animator = m_property_animators.emplace(entity, m_allocator);
		animator.animation = nullptr;
		animator.time = 0;
		m_universe.onComponentCreated(entity, PROPERTY_ANIMATOR_TYPE, this);
	}


	void createAnimable(EntityRef entity)
	{
		Animable& animable = m_animables.insert(entity);
		animable.time = Time::fromSeconds(0);
		animable.animation = nullptr;
		animable.entity = entity;

		m_universe.onComponentCreated(entity, ANIMABLE_TYPE, this);
	}


	void createAnimator(EntityRef entity)
	{
		m_animator_map.insert(entity, m_animators.size());
		Animator& animator = m_animators.emplace();
		animator.entity = entity;

		m_universe.onComponentCreated(entity, ANIMATOR_TYPE, this);
	}


	IPlugin& getPlugin() const override { return m_anim_system; }


	IAllocator& m_allocator;
	Universe& m_universe;
	IPlugin& m_anim_system;
	Engine& m_engine;
	AssociativeArray<EntityRef, Animable> m_animables;
	AssociativeArray<EntityRef, PropertyAnimator> m_property_animators;
	HashMap<EntityRef, u32> m_animator_map;
	Array<Animator> m_animators;
	RenderScene* m_render_scene;
	bool m_is_game_running;
	OutputMemoryStream m_event_stream;
};


AnimationScene* AnimationScene::create(Engine& engine, IPlugin& plugin, Universe& universe, IAllocator& allocator)
{
	return LUMIX_NEW(allocator, AnimationSceneImpl)(engine, plugin, universe, allocator);
}


void AnimationScene::destroy(AnimationScene& scene)
{
	AnimationSceneImpl& scene_impl = (AnimationSceneImpl&)scene;
	LUMIX_DELETE(scene_impl.m_allocator, &scene_impl);
}


void AnimationScene::registerLuaAPI(lua_State* L)
{
	#define REGISTER_FUNCTION(name) \
	do {\
		auto f = &LuaWrapper::wrapMethod<&AnimationSceneImpl::name>; \
		LuaWrapper::createSystemFunction(L, "Animation", #name, f); \
	} while(false) \

	REGISTER_FUNCTION(getAnimationLength);
	REGISTER_FUNCTION(setAnimatorU32Input);
	REGISTER_FUNCTION(setAnimatorBoolInput);
	REGISTER_FUNCTION(setAnimatorFloatInput);
	REGISTER_FUNCTION(getAnimatorInputIndex);

	#undef REGISTER_FUNCTION

	LuaWrapper::createSystemFunction(L, "Animation", "setIK", &AnimationSceneImpl::setIK); \

}


}
