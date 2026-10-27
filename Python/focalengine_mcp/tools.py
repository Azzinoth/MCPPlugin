"""Built-in tools, their handlers run on main thread, because engine could be used only there."""
import focalengine

# Engine and editor mark their own objects with these tags, tools neither list them nor change them.
PRIVATE_TAGS = frozenset(("EDITOR_PRIVATE_RESOURCE", "ENGINE_PRIVATE_RESOURCE", "PREFAB_DESCRIPTION"))
DEFAULT_LIST_LIMIT = 200
MAXIMUM_LIST_LIMIT = 1000


class ToolError(Exception):
    """Returned to client as tool result with is_error set, so model could correct its call."""


class ToolOutput:
    """Tool result with text and optional image, built-in tools return dictionaries that are sent as JSON text instead."""

    def __init__(self, text, is_error=False, image_base64="", image_mime_type=""):
        self.text = text
        self.is_error = is_error
        self.image_base64 = image_base64
        self.image_mime_type = image_mime_type


class Tool:
    def __init__(self, name, description, input_schema, handler, annotations=None):
        self.name = name
        self.description = description
        self.input_schema = input_schema
        self.handler = handler
        # Hints for clients, tools registered from C++ have none, because their behavior is not known.
        self.annotations = annotations
        # Created on server thread when tool is called first time.
        self.validator = None


READ_ONLY_HINTS = {"read_only_hint": True}


SCENE_ID_SCHEMA = {
    "type": "string",
    "description": "Scene ID from list_scenes. Default is the scene focused in the editor, otherwise the starting scene.",
}
ENTITY_ID_SCHEMA = {"type": "string", "description": "Entity ID from list_entities."}
SPACE_SCHEMA = {
    "type": "string",
    "enum": ["local", "world"],
    "default": "local",
    "description": "local is relative to parent entity, world is relative to scene. World values are recomputed later in the frame, so they could be old right after a change.",
}
ALLOW_PLAY_MODE_SCHEMA = {
    "type": "boolean",
    "default": False,
    "description": "Scene with GameMode flag is a play mode copy, its changes are lost when play mode stops. It is changed only when this is true.",
}


def _get_name(engine_object):
    try:
        return engine_object.get_name()
    except UnicodeDecodeError:
        return "<name is not valid UTF-8>"


def _is_private_scene(scene):
    return scene.get_tag() in PRIVATE_TAGS or "PrefabDescription" in scene.get_flags()


def _get_focused_scene():
    editor = focalengine.editor
    if editor.is_available() and editor.is_project_open():
        return editor.get_focused_scene()

    return None


def _get_scene(scene_id):
    if scene_id is None:
        scene = _get_focused_scene() or focalengine.get_starting_scene()
        if scene is None:
            raise ToolError("No scene is open")

        return scene

    scene = focalengine.get_scene(scene_id)
    if scene is None or _is_private_scene(scene):
        raise ToolError(f"Scene {scene_id} does not exist, list_scenes shows existing scenes")

    return scene


def resolve_entity(arguments, for_write):
    scene = _get_scene(arguments.get("scene_id"))
    entity_id = arguments["entity_id"]
    entity = scene.get_entity(entity_id)
    if entity is None:
        raise ToolError(f"Entity {entity_id} does not exist in scene {scene.id}, list_entities shows entities of a scene")

    if for_write:
        if entity.get_tag() in PRIVATE_TAGS:
            raise ToolError(f"Entity {entity_id} belongs to engine or editor and could not be changed")

        if "GameMode" in scene.get_flags() and not arguments.get("allow_play_mode", False):
            raise ToolError(f"Scene {scene.id} is a play mode copy, its changes are lost when play mode stops, pass allow_play_mode=true to change it anyway")

    return scene, entity


def _mark_project_modified(scene):
    editor = focalengine.editor
    if editor.is_available() and editor.is_project_open() and "GameMode" not in scene.get_flags():
        editor.mark_project_modified()


def _use_world_space(arguments):
    if arguments.get("space", "local") != "world":
        return False

    if focalengine.get_graphics_mode() == "None":
        raise ToolError("World space values are not updated in applications without graphics, use local space")

    return True


def _to_list(vector):
    return [round(vector.x, 6), round(vector.y, 6), round(vector.z, 6)]


def list_scenes(arguments):
    starting_scene = focalengine.get_starting_scene()
    focused_scene = _get_focused_scene()
    scenes = []
    for scene in focalengine.get_scenes():
        try:
            if _is_private_scene(scene):
                continue

            scenes.append({
                "id": scene.id,
                "name": _get_name(scene),
                "flags": scene.get_flags(),
                "is_starting": starting_scene is not None and scene.id == starting_scene.id,
                "is_focused": focused_scene is not None and scene.id == focused_scene.id,
            })
        except Exception as error:
            scenes.append({"id": scene.id, "error": f"{type(error).__name__}: {error}"})

    return {"scenes": scenes}


def list_entities(arguments):
    scene = _get_scene(arguments.get("scene_id"))
    name_filter = arguments.get("name_filter", "").lower()
    offset = arguments.get("offset", 0)
    limit = arguments.get("limit", DEFAULT_LIST_LIMIT)

    entities = []
    for entity in scene.get_entities():
        try:
            if entity.get_tag() in PRIVATE_TAGS:
                continue

            name = _get_name(entity)
            if name_filter and name_filter not in name.lower():
                continue

            parent = entity.get_parent()
            entities.append({"id": entity.id, "name": name, "parent_id": parent.id if parent is not None else None})
        except Exception as error:
            entities.append({"id": entity.id, "error": f"{type(error).__name__}: {error}"})

    page = entities[offset:offset + limit]
    return {"scene_id": scene.id, "total": len(entities), "offset": offset, "entities": page, "truncated": offset + len(page) < len(entities)}


def get_entity_transform(arguments):
    scene, entity = resolve_entity(arguments, for_write=False)
    world = _use_world_space(arguments)
    return {
        "scene_id": scene.id,
        "entity_id": entity.id,
        "space": "world" if world else "local",
        "position": _to_list(entity.get_position(world)),
        "rotation": _to_list(entity.get_rotation(world)),
        "scale": _to_list(entity.get_scale(world)),
    }


def set_entity_position(arguments):
    scene, entity = resolve_entity(arguments, for_write=True)
    world = _use_world_space(arguments)
    position = focalengine.Vector3(arguments["x"], arguments["y"], arguments["z"])
    entity.set_position(position, world)
    _mark_project_modified(scene)
    return {"scene_id": scene.id, "entity_id": entity.id, "space": "world" if world else "local", "position": _to_list(position)}


BUILT_IN_TOOLS = [
    Tool(
        "list_scenes",
        "Lists scenes. Result: {\"scenes\": [{\"id\", \"name\", \"flags\", \"is_starting\", \"is_focused\"}]}. "
        "Scene with \"GameMode\" flag is a play mode copy of other scene.",
        {"type": "object", "properties": {}, "additionalProperties": False},
        list_scenes,
        READ_ONLY_HINTS,
    ),
    Tool(
        "list_entities",
        "Lists entities of a scene. Result: {\"scene_id\", \"total\", \"offset\", \"entities\": [{\"id\", \"name\", \"parent_id\"}], \"truncated\"}.",
        {
            "type": "object",
            "properties": {
                "scene_id": SCENE_ID_SCHEMA,
                "name_filter": {"type": "string", "description": "Only entities whose name contains this text, case is ignored."},
                "offset": {"type": "integer", "minimum": 0, "default": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": MAXIMUM_LIST_LIMIT, "default": DEFAULT_LIST_LIMIT},
            },
            "additionalProperties": False,
        },
        list_entities,
        READ_ONLY_HINTS,
    ),
    Tool(
        "get_entity_transform",
        "Gets position, rotation and scale of an entity. Rotation is Euler angles in degrees, applied around X, then Y, then Z. "
        "Result: {\"scene_id\", \"entity_id\", \"space\", \"position\": [x, y, z], \"rotation\": [x, y, z], \"scale\": [x, y, z]}.",
        {
            "type": "object",
            "properties": {"entity_id": ENTITY_ID_SCHEMA, "scene_id": SCENE_ID_SCHEMA, "space": SPACE_SCHEMA},
            "required": ["entity_id"],
            "additionalProperties": False,
        },
        get_entity_transform,
        READ_ONLY_HINTS,
    ),
    Tool(
        "set_entity_position",
        "Moves an entity to absolute position. Result: {\"scene_id\", \"entity_id\", \"space\", \"position\": [x, y, z]}.",
        {
            "type": "object",
            "properties": {
                "entity_id": ENTITY_ID_SCHEMA,
                "x": {"type": "number"},
                "y": {"type": "number"},
                "z": {"type": "number"},
                "scene_id": SCENE_ID_SCHEMA,
                "space": SPACE_SCHEMA,
                "allow_play_mode": ALLOW_PLAY_MODE_SCHEMA,
            },
            "required": ["entity_id", "x", "y", "z"],
            "additionalProperties": False,
        },
        set_entity_position,
        {"read_only_hint": False, "destructive_hint": False, "idempotent_hint": True},
    ),
]