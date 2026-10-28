#pragma once
#include <functional>
#include <string>
#include <vector>

// Public API of MCPPlugin, applications include only this header.
// Usage:
//	using PluginInterface = MCPPluginInterface;
//	PluginInterface* Plugin = PLUGIN_MANAGER.GetPlugin<PluginInterface>(UNIQUE_ID.FromString(PluginInterface::PluginID));
//	if (Plugin != nullptr && Plugin->GetInterfaceVersion() >= PluginInterface::InterfaceVersion)
//		Plugin->...;
// Functions are only appended, so plugin with newer version works with code that was built with older header.
class MCPPluginInterface
{
public:
	// Same as "ID" in MCPPlugin.json.
	static constexpr const char* PluginID = "beba13e8-8e0f-4fcd-8d5b-bb3b67a19066";

	// Increase when anything in this file changes.
	static constexpr int InterfaceVersion = 2;

	// Version 2.
	struct ToolResult
	{
		// Error text is shown to the model, so it could correct its call.
		bool bIsError = false;
		std::string Text;
		// Optional image sent together with Text, RGBA with 8 bits per channel, rows go from top to bottom.
		// Plugin scales it down and compresses it to JPEG, because clients reject large tool results.
		std::vector<unsigned char> ImagePixels;
		int ImageWidth = 0;
		int ImageHeight = 0;
	};
	using ToolHandler = std::function<ToolResult(const std::string& ArgumentsJSON)>;

	// Order of virtual functions is part of the interface, new functions should be added only at the end.
	virtual ~MCPPluginInterface() = default;
	virtual int GetInterfaceVersion() const { return InterfaceVersion; }

	// Version 2.
	// Functions below are main thread only.
	// Server starts by itself in applications that load plugin as DLL, for example editor. Applications that link plugin statically start it themselves.
	virtual bool StartServer(int Port) = 0;
	virtual void StopServer() = 0;
	virtual bool IsServerRunning() = 0;
	// Empty when server is not running.
	virtual std::string GetServerURL() = 0;

	// Handler runs on main thread. Module should unregister its tools with UnregisterToolsOfOwner before it is unloaded.
	// Name has 1 to 46 characters from A-Z, a-z, 0-9, _ and -. InputJSONSchema is JSON object with "type": "object" at root.
	virtual bool RegisterTool(const std::string& OwnerName, const std::string& Name, const std::string& Description, const std::string& InputJSONSchema, ToolHandler Handler) = 0;
	virtual bool UnregisterTool(const std::string& Name) = 0;
	virtual void UnregisterToolsOfOwner(const std::string& OwnerName) = 0;
};