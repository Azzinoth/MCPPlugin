#pragma once
#include "SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.h"
// Copy of PythonPlugin public header, MCP server runs in Python that PythonPlugin provides.
#include "SubSystems/PythonPlugin/PythonPluginInterface.h"
#include "Public/MCPPluginInterface.h"
#include <map>
#include <memory>
#include <set>
#include <thread>
using namespace FocalEngine;

class MCPPlugin : public FEPluginCore, public MCPPluginInterface
{
public:
	bool OnLoad() override;
	void OnUnload() override;
	void OnUpdate(double DeltaTime) override;

	bool StartServer(int Port) override;
	void StopServer() override;
	bool IsServerRunning() override;
	std::string GetServerURL() override;

	bool RegisterTool(const std::string& OwnerName, const std::string& Name, const std::string& Description, const std::string& InputJSONSchema, ToolHandler Handler) override;
	bool UnregisterTool(const std::string& Name) override;
	void UnregisterToolsOfOwner(const std::string& OwnerName) override;
private:
	bool bServerStarted = false;
	int ServerPort = 0;
	std::thread::id MainThreadID;
	// Python server reaches tools registered from C++ through native functions of PythonPlugin.
	bool bNativeFunctionsRegistered = false;
	bool bToolsChanged = false;
	// Names of built-in tools of Python server, they are known after server started first time.
	std::set<std::string> ReservedToolNames;

	struct RegisteredTool
	{
		std::string OwnerName;
		std::string Description;
		Json::Value InputSchema;
		ToolHandler Handler;
	};
	// Allocated separately, so tools that owners did not unregister could be leaked on unload,
	// destroying their handlers could run code of modules that are already unloaded.
	std::unique_ptr<std::map<std::string, RegisteredTool>> Tools = std::make_unique<std::map<std::string, RegisteredTool>>();

	bool IsMainThread() const;
	// Returns nullptr if PythonPlugin is not loaded or Python failed to start.
	PythonPluginInterface* FindPython();

	void RegisterNativeFunctions(PythonPluginInterface* Python);
	std::string ListToolsAsJSON() const;
	std::string CallToolFromJSON(const std::string& RequestJSON);
	void SetReservedToolNames(const std::string& NamesJSON);
};