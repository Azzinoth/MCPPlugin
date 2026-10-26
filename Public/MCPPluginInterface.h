#pragma once

// Public API of MCPPlugin, applications include only this header.
// Usage:
//	using PluginInterface = MCPPluginInterface;
//	PluginInterface* Plugin = PLUGIN_MANAGER.GetPlugin<PluginInterface>(UNIQUE_ID.FromString(PluginInterface::PluginID));
//	if (Plugin != nullptr && Plugin->GetInterfaceVersion() == PluginInterface::InterfaceVersion)
//		Plugin->...;
class MCPPluginInterface
{
public:
	// Same as "ID" in MCPPlugin.json.
	static constexpr const char* PluginID = "beba13e8-8e0f-4fcd-8d5b-bb3b67a19066";

	// Increase when anything in this file changes.
	static constexpr int InterfaceVersion = 1;

	// Order of virtual functions is part of the interface, new functions should be added only at the end.
	virtual ~MCPPluginInterface() = default;
	virtual int GetInterfaceVersion() const { return InterfaceVersion; }
};