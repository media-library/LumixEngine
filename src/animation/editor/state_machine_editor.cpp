#include "state_machine_editor.h"
#include "animation/animation.h"
#include "animation/editor/animation_editor.h"
#include "animation/controller.h"
#include "animation/state_machine.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/resource_manager.h"
#include "engine/resource_manager_base.h"
#include <cmath>


static const Lumix::ResourceType CONTROLLER_RESOURCE_TYPE("anim_controller");


using namespace Lumix;


static ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x + b.x, a.y + b.y);
}


static ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x - b.x, a.y - b.y);
}


static ImVec2 operator*(const ImVec2& a, float b)
{
	return ImVec2(a.x * b, a.y * b);
}


static float dot(const ImVec2& a, const ImVec2& b)
{
	return a.x * b.x + a.y * b.y;
}


namespace AnimEditor
{


static ImVec2 getEdgeStartPoint(Node* a, Node* b, bool is_dir)
{
	ImVec2 center_a = a->pos + a->size * 0.5f;
	ImVec2 center_b = b->pos + b->size * 0.5f;
	ImVec2 dir = center_b - center_a;
	if (fabs(dir.x / dir.y) > fabs(a->size.x / a->size.y))
	{
		dir = dir * fabs(1 / dir.x);
		return center_a + dir * a->size.x * 0.5f + ImVec2(0, center_a.y > center_b.y == is_dir ? 5.0f : -5.0f);
	}

	dir = dir * fabs(1 / dir.y);
	return center_a + dir * a->size.y * 0.5f + ImVec2(center_a.x > center_b.x == is_dir ? 5.0f : -5.0f, 0);
}


Node::Node(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Component(parent, engine_cmp)
	, edges(controller.getAllocator())
	, m_allocator(controller.getAllocator())
	, m_controller(controller)
{
	m_name[0] = 0;
}


bool Node::hitTest(const ImVec2& on_canvas_pos) const
{
	return on_canvas_pos.x >= pos.x && on_canvas_pos.x < pos.x + size.x
		&& on_canvas_pos.y >= pos.y && on_canvas_pos.y < pos.y + size.y;
}


void Node::onGUI()
{
	ImGui::InputText("Name", m_name, lengthOf(m_name));
}


void Node::serialize(OutputBlob& blob)
{
	blob.write(pos);
	blob.write(size);
	blob.write(m_name);
}


void Node::deserialize(InputBlob& blob)
{
	blob.read(pos);
	blob.read(size);
	blob.read(m_name);
}


bool Node::draw(ImDrawList* draw, const ImVec2& canvas_screen_pos, bool selected)
{

	ImGui::PushID(engine_cmp);
	ImVec2 from = canvas_screen_pos + pos;
	ImVec2 to = from + size;
	draw->AddRectFilled(from,
		to,
		ImGui::ColorConvertFloat4ToU32(selected ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : ImGui::GetStyle().Colors[ImGuiCol_Button]));

	ImGui::SetCursorScreenPos(from);
	ImGui::Text("%s", m_name);

	ImGui::SetCursorScreenPos(from);
	ImGui::InvisibleButton("bg", size);
	ImGui::PopID();
	return ImGui::IsItemActive();
}


Container::Container(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Node(engine_cmp, parent, controller)
	, m_editor_cmps(controller.getAllocator())
	, m_selected_component(nullptr)
{
}


Component* Container::childrenHitTest(const ImVec2& pos)
{
	for (auto* i : m_editor_cmps)
	{
		if (i->hitTest(pos)) return i;
	}
	return nullptr;
}


Component* Container::getChildByUID(int uid)
{
	for (auto* i : m_editor_cmps)
	{
		if (i->engine_cmp->uid == uid) return i;
	}
	return nullptr;
}


Edge::Edge(Anim::Edge* engine_cmp, Container* parent, ControllerResource& controller)
	: Component(parent, engine_cmp)
	, m_controller(controller)
{
	m_from = (Node*)parent->getChildByUID(engine_cmp->from->uid);
	m_to = (Node*)parent->getChildByUID(engine_cmp->to->uid);
	ASSERT(m_from);
	ASSERT(m_to);
	m_expression[0] = 0;
}


void Edge::onGUI()
{
	auto* engine_edge = (Anim::Edge*)engine_cmp;
	ImGui::DragFloat("Length", &engine_edge->length);
	if (ImGui::InputText("Expression", m_expression, lengthOf(m_expression)))
	{
		engine_edge->condition.compile(m_expression, m_controller.getEngineResource()->getInputDecl());
	}
}


bool Edge::draw(ImDrawList* draw, const ImVec2& canvas_screen_pos, bool selected)
{
	uint32 color = ImGui::ColorConvertFloat4ToU32(
		selected ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : ImGui::GetStyle().Colors[ImGuiCol_Button]);
	draw->AddLine(getEdgeStartPoint(m_from, m_to, true) + canvas_screen_pos,
		getEdgeStartPoint(m_to, m_from, false) + canvas_screen_pos,
		color);
	if (ImGui::IsMouseClicked(0) && hitTest(ImGui::GetMousePos() - canvas_screen_pos))
	{
		return true;
	}
	return false;
}


void Edge::serialize(OutputBlob& blob)
{
	blob.write(m_from->engine_cmp->uid);
	blob.write(m_to->engine_cmp->uid);
	blob.write(m_expression);
}


void Edge::deserialize(InputBlob& blob)
{
	int uid;
	blob.read(uid);
	m_from = (Node*)m_parent->getChildByUID(uid);
	blob.read(uid);
	m_to = (Node*)m_parent->getChildByUID(uid);
	blob.read(m_expression);
}


bool Edge::hitTest(const ImVec2& on_canvas_pos) const
{
	ImVec2 a = getEdgeStartPoint(m_from, m_to, true);
	ImVec2 b = getEdgeStartPoint(m_to, m_from, false);

	ImVec2 dif = a - b;
	float len_squared = dif.x * dif.x + dif.y * dif.y;
	float t = Math::clamp(dot(on_canvas_pos - a, b - a) / len_squared, 0.0f, 1.0f);
	const ImVec2 projection = a + (b - a) * t;
	ImVec2 dist_vec = on_canvas_pos - projection;

	return dot(dist_vec, dist_vec) < 100;
}


SimpleAnimationNode::SimpleAnimationNode(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Node(engine_cmp, parent, controller)
{
	animation[0] = 0;
}


void SimpleAnimationNode::onGUI()
{
	Node::onGUI();
	
	auto* node = (Anim::SimpleAnimationNode*)engine_cmp;
	auto getter = [](void* data, int idx, const char** out) -> bool {
		auto* node = (SimpleAnimationNode*)data;
		auto& slots = node->m_controller.getAnimationSlots();
		*out = slots[idx].c_str();
		return true;
	};
	
	auto& slots = m_controller.getAnimationSlots();
	int current = 0;
	for (current = 0; current < slots.size() && crc32(slots[current].c_str()) != node->animation_hash ; ++current);
	if (ImGui::Combo("Animation", &current, getter, this, slots.size()))
	{
		node->animation_hash = crc32(slots[current].c_str());
	}
}


StateMachine::StateMachine(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
	: Container(engine_cmp, parent, controller)
{
}


void StateMachine::onGUI()
{
	Container::onGUI();
	if (ImGui::Button("Show Children"))
	{
		m_controller.getEditor().setContainer(this);
	}
}


static Component* createComponent(Anim::Component* engine_cmp, Container* parent, ControllerResource& controller)
{
	IAllocator& allocator = controller.getAllocator();
	switch (engine_cmp->type)
	{
		case Anim::Component::EDGE: return LUMIX_NEW(allocator, Edge)((Anim::Edge*)engine_cmp, parent, controller);
		case Anim::Component::SIMPLE_ANIMATION:
			return LUMIX_NEW(allocator, SimpleAnimationNode)(engine_cmp, parent, controller);
		case Anim::Component::STATE_MACHINE: return LUMIX_NEW(allocator, StateMachine)(engine_cmp, parent, controller);
		default: ASSERT(false); return nullptr;
	}
}


void StateMachine::deserialize(InputBlob& blob)
{
	ASSERT(m_editor_cmps.empty());
	int size;
	blob.read(size);
	for (int i = 0; i < size; ++i)
	{
		int uid;
		blob.read(uid);
		auto* engine_sm = (Anim::StateMachine*)engine_cmp;
		Component* cmp = createComponent(engine_sm->getChildByUID(uid), this, m_controller);
		cmp->deserialize(blob);
		m_editor_cmps.push(cmp);
	}
}


void StateMachine::serialize(OutputBlob& blob)
{
	blob.write(m_editor_cmps.size());
	for (auto* cmp : m_editor_cmps)
	{
		blob.write(cmp->engine_cmp->uid);
		cmp->serialize(blob);
	}
}


void StateMachine::createState(Anim::Component::Type type)
{
	auto* cmp = (Node*)createComponent(Anim::createComponent(type, m_allocator), this, m_controller);
	cmp->size.x = 100;
	cmp->size.y = 30;
	cmp->engine_cmp->uid = m_controller.createUID();
	m_editor_cmps.push(cmp);
	((Anim::StateMachine*)engine_cmp)->children.push(cmp->engine_cmp);
	m_selected_component = cmp;
}


void StateMachine::drawInside(ImDrawList* draw, const ImVec2& canvas_screen_pos)
{
	if (ImGui::IsWindowHovered())
	{
		if (ImGui::IsMouseClicked(0)) m_selected_component = nullptr;
		if (ImGui::IsMouseReleased(1) && !m_is_making_line)
		{
			ImGui::OpenPopup("context_menu");
		}
	}
	for (int i = 0; i < m_editor_cmps.size(); ++i)
	{
		Component* cmp = m_editor_cmps[i];
		if (cmp->draw(draw, canvas_screen_pos, m_selected_component == cmp))
		{
			m_selected_component = cmp;
		}

		if (m_selected_component == cmp && cmp->isNode())
		{
			Node* node = (Node*)cmp;
			
			if (ImGui::IsMouseReleased(1) && m_is_making_line)
			{
				m_is_making_line = false;
				Component* target = childrenHitTest(ImGui::GetMousePos() - canvas_screen_pos);
				if (target && target != m_selected_component && target->isNode())
				{
					auto* engine_parent = ((Anim::Container*)engine_cmp);
					auto* engine_edge = LUMIX_NEW(m_allocator, Anim::Edge)(m_allocator);
					engine_edge->uid = m_controller.createUID();
					engine_edge->from = (Anim::Node*)m_selected_component->engine_cmp;
					engine_edge->to = (Anim::Node*)target->engine_cmp;
					engine_parent->children.push(engine_edge);

					auto* edge = LUMIX_NEW(m_allocator, Edge)(engine_edge, this, m_controller);
					m_editor_cmps.push(edge);
				}
			}
			
			if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(1)) m_is_making_line = true;
			if (m_is_making_line)
			{
				draw->AddLine(canvas_screen_pos + node->pos + node->size * 0.5f, ImGui::GetMousePos(), 0xfff00FFF);
			}

			if (ImGui::IsMouseDragging())
			{
				node->pos = node->pos + ImGui::GetIO().MouseDelta;
			}
		}
	}
	if (ImGui::BeginPopup("context_menu"))
	{
		if (ImGui::BeginMenu("Create"))
		{
			if (ImGui::MenuItem("Simple")) createState(Anim::Component::SIMPLE_ANIMATION);
			if (ImGui::MenuItem("State machine")) createState(Anim::Component::STATE_MACHINE);
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}
}


ControllerResource::ControllerResource(AnimationEditor& editor, ResourceManagerBase& manager, IAllocator& allocator)
	: m_animation_slots(allocator)
	, m_allocator(allocator)
	, m_editor(editor)
{
	m_engine_resource = LUMIX_NEW(allocator, Anim::ControllerResource)(Path("editor"), manager, allocator);
	auto* engine_root = LUMIX_NEW(allocator, Anim::StateMachine)(allocator);
	m_engine_resource->setRoot(engine_root);
	m_root = LUMIX_NEW(allocator, StateMachine)(engine_root, nullptr, *this);
}


void ControllerResource::serialize(OutputBlob& blob)
{
	blob.write(m_last_uid);
	m_engine_resource->serialize(blob);
	m_root->serialize(blob);
	blob.write(m_animation_slots.size());
	for (auto& slot : m_animation_slots)
	{
		blob.writeString(slot.c_str());
	}
}


void ControllerResource::deserialize(InputBlob& blob, Engine& engine, IAllocator& allocator)
{
	blob.read(m_last_uid);
	auto* manager = engine.getResourceManager().get(CONTROLLER_RESOURCE_TYPE);
	m_engine_resource = LUMIX_NEW(allocator, Anim::ControllerResource)(Path("editor"), *manager, allocator);
	m_engine_resource->create();
	m_engine_resource->deserialize(blob);

	m_root = createComponent(m_engine_resource->getRoot(), nullptr, *this);
	m_root->deserialize(blob);

	int count;
	blob.read(count);
	m_animation_slots.clear();
	for (int i = 0; i < count; ++i)
	{
		auto& slot = m_animation_slots.emplace(allocator);
		char tmp[64];
		blob.readString(tmp, lengthOf(tmp));
		slot = tmp;
	}
}


} // namespace AnimEditor