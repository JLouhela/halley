#include "undo_stack.h"

#include "scene_editor_window.h"
using namespace Halley;

UndoStack::UndoStack()
	: maxSize(50)
	, accepting(false)
{
}

void UndoStack::pushAdded(const String& entityId, const String& parent, int childIndex, const EntityData& data)
{
	if (accepting) {
		addToStack(Action(Type::EntityAdded, EntityDataDelta(data), entityId, parent, childIndex), Action(Type::EntityRemoved, EntityDataDelta(), entityId));
	}
}

void UndoStack::pushRemoved(const String& entityId, const String& parent, int childIndex, const EntityData& data)
{
	if (accepting) {
		addToStack(Action(Type::EntityRemoved, EntityDataDelta(), entityId), Action(Type::EntityAdded, EntityDataDelta(data), entityId, parent, childIndex));
	}
}

void UndoStack::pushMoved(const String& entityId, const String& prevParent, int prevIndex, const String& newParent, int newIndex)
{
	if (accepting) {
		addToStack(Action(Type::EntityMoved, EntityDataDelta(), entityId, newParent, newIndex), Action(Type::EntityMoved, EntityDataDelta(), entityId, prevParent, prevIndex));
	}
}

void UndoStack::pushModified(const String& entityId, const EntityData& before, const EntityData& after)
{
	if (accepting) {
		addToStack(Action(Type::EntityModified, EntityDataDelta(before, after), entityId), Action(Type::EntityModified, EntityDataDelta(after, before), entityId));
	}
}

void UndoStack::undo(SceneEditorWindow& sceneEditorWindow)
{
	if (stackPos > 0) {
		runAction(stack[--stackPos]->back, sceneEditorWindow);
	}
}

void UndoStack::redo(SceneEditorWindow& sceneEditorWindow)
{
	if (stackPos < stack.size()) {
		runAction(stack[stackPos++]->forward, sceneEditorWindow);
	}
}

void UndoStack::addToStack(Action forward, Action back)
{
	// Discard redo timeline that is no longer valid
	if (stack.size() > stackPos) {
		stack.resize(stackPos);
	}
	
	stack.emplace_back(std::make_unique<ActionPair>(forward, back));
	if (stack.size() > maxSize) {
		stack.erase(stack.begin());
	}
	stackPos = stack.size();
}

void UndoStack::runAction(const Action& action, SceneEditorWindow& sceneEditorWindow)
{
	accepting = false;

	switch (action.type) {
	case Type::EntityAdded:
		sceneEditorWindow.addEntity(action.parent, action.childIndex, EntityData(action.delta));
		break;

	case Type::EntityRemoved:
		sceneEditorWindow.removeEntity(action.entityId);
		break;
		
	case Type::EntityMoved:
		sceneEditorWindow.moveEntity(action.entityId, action.parent, action.childIndex);
		break;

	case Type::EntityModified:
		sceneEditorWindow.modifyEntity(action.entityId, action.delta);
		break;
	}
	
	accepting = true;
}
