#include "MCPPlugin.h"
#include <cctype>
#include <cstdlib>

// DO NOT CHANGE THIS LINE.
// The plugin manager uses it to create and destroy the plugin instance.
REGISTER_PLUGIN(MCPPlugin)

namespace
{
	const int DefaultPort = 5187;
	// Clients show tool as mcp__focalengine__<name>, with this limit that full name fits in 64 characters.
	const size_t MaximumToolNameLength = 46;
	const size_t MaximumPropertyNameLength = 64;

	// Other port could be set, for example when other application already uses default one.
	int GetPort()
	{
		char* Value = nullptr;
		size_t Length = 0;
		if (_dupenv_s(&Value, &Length, "FOCAL_ENGINE_MCP_PORT") != 0 || Value == nullptr)
			return DefaultPort;

		const std::string Text = Value;
		free(Value);

		char* End = nullptr;
		const long Port = std::strtol(Text.c_str(), &End, 10);
		if (End == Text.c_str() || *End != '\0' || Port < 1 || Port > 65535)
		{
			LOG.Add("MCPPlugin: FOCAL_ENGINE_MCP_PORT value \"" + Text + "\" is not a valid port, port " + std::to_string(DefaultPort) + " is used", "MCP_PLUGIN", FE_LOG_WARNING);
			return DefaultPort;
		}

		return static_cast<int>(Port);
	}

	void LogExecutionError(const std::string& FunctionName, const PythonPluginInterface::ExecutionResult& Result)
	{
		std::string Message = "MCPPlugin::" + FunctionName + ": " + Result.Error;
		if (!Result.Output.empty())
			Message += "\nOutput:\n" + Result.Output;

		LOG.Add(Message, "MCP_PLUGIN", FE_LOG_ERROR);
	}

	bool IsValidName(const std::string& Name, size_t MaximumLength, bool bAllowDots)
	{
		if (Name.empty() || Name.size() > MaximumLength)
			return false;

		for (const char Character : Name)
		{
			if (!std::isalnum(static_cast<unsigned char>(Character)) && Character != '_' && Character != '-' && !(bAllowDots && Character == '.'))
				return false;
		}

		return true;
	}

	// Clients reject whole tool list when one schema breaks their rules, so tool with such schema is not registered.
	bool IsValidInputSchema(const std::string& InputJSONSchema, std::string& OutError)
	{
		Json::Value Root;
		std::string ParseErrors;
		Json::CharReaderBuilder Builder;
		const std::unique_ptr<Json::CharReader> Reader(Builder.newCharReader());
		if (!Reader->parse(InputJSONSchema.c_str(), InputJSONSchema.c_str() + InputJSONSchema.size(), &Root, &ParseErrors))
		{
			OutError = "it is not valid JSON: " + ParseErrors;
			return false;
		}

		if (!Root.isObject() || !Root.isMember("type") || !Root["type"].isString() || Root["type"].asString() != "object")
		{
			OutError = "it should be JSON object with \"type\": \"object\"";
			return false;
		}

		if (Root.isMember("anyOf") || Root.isMember("oneOf") || Root.isMember("allOf"))
		{
			OutError = "anyOf, oneOf and allOf are not allowed at root";
			return false;
		}

		if (!Root.isMember("properties"))
			return true;

		if (!Root["properties"].isObject())
		{
			OutError = "\"properties\" should be JSON object";
			return false;
		}

		const std::vector<std::string> PropertyNames = Root["properties"].getMemberNames();
		for (size_t i = 0; i < PropertyNames.size(); i++)
		{
			if (!IsValidName(PropertyNames[i], MaximumPropertyNameLength, true))
			{
				OutError = "property name \"" + PropertyNames[i] + "\" should have 1 to 64 characters from A-Z, a-z, 0-9, _, - and .";
				return false;
			}
		}

		return true;
	}
}

void MCPPlugin::OnLoad()
{
	MainThreadID = std::this_thread::get_id();

	// Applications that link plugin statically, for example built games, decide themselves whether server starts, so they do not open a port by default.
#ifdef FOCAL_ENGINE_SHARED
	StartServer(GetPort());
#endif
}

void MCPPlugin::OnUnload()
{
	StopServer();

	// Owners unregister their tools before they are unloaded, handlers that are left could belong to modules that are already unloaded.
	if (!Tools->empty())
	{
		std::string ToolNames;
		for (const auto& Tool : *Tools)
			ToolNames += (ToolNames.empty() ? "\"" : ", \"") + Tool.first + "\" of \"" + Tool.second.OwnerName + "\"";

		LOG.Add("MCPPlugin::OnUnload: Tools " + ToolNames + " were not unregistered, they are leaked, because destroying their handlers could run code of unloaded modules", "MCP_PLUGIN", FE_LOG_WARNING);
		static_cast<void>(Tools.release());
		Tools = std::make_unique<std::map<std::string, RegisteredTool>>();
	}
}

bool MCPPlugin::IsMainThread() const
{
	return std::this_thread::get_id() == MainThreadID;
}

PythonPluginInterface* MCPPlugin::FindPython()
{
	PythonPluginInterface* Python = PLUGIN_MANAGER.GetPlugin<PythonPluginInterface>(UNIQUE_ID.FromString(PythonPluginInterface::PluginID));
	if (Python == nullptr || !Python->IsInitialized())
		return nullptr;

	return Python;
}

bool MCPPlugin::StartServer(int Port)
{
	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because it should be started on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (Port < 1 || Port > 65535)
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because " + std::to_string(Port) + " is not a valid port", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (bServerStarted)
	{
		if (Port == ServerPort)
			return true;

		LOG.Add("MCPPlugin::StartServer: MCP server already runs on port " + std::to_string(ServerPort) + ", it should be stopped before it is started on other port", "MCP_PLUGIN", FE_LOG_WARNING);
		return false;
	}

	PythonPluginInterface* Python = PLUGIN_MANAGER.GetPlugin<PythonPluginInterface>(UNIQUE_ID.FromString(PythonPluginInterface::PluginID));
	if (Python == nullptr)
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because PythonPlugin is not loaded", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	// Server itself needs only version 1.
	if (Python->GetInterfaceVersion() < 1)
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because PythonPlugin interface version " + std::to_string(Python->GetInterfaceVersion()) + " is not supported", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (!Python->IsInitialized())
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because Python failed to start, PYTHON_PLUGIN log has the reason", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	// Binary modules of vendored packages are built only for this Python version.
	if (Python->GetPythonVersion().rfind("3.13.", 0) != 0)
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because its packages are built for Python 3.13, but PythonPlugin has Python " + Python->GetPythonVersion(), "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	const std::string PythonDirectory = GetPluginDirectory() + "Data/Python";
	if (!Python->AddModuleSearchPath(PythonDirectory))
	{
		LOG.Add("MCPPlugin::StartServer: MCP server is not started, because \"" + PythonDirectory + "\" could not be added to Python module search path", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	// Returns right away, server reports to log when it listens.
	const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.start(port=" + std::to_string(Port) + ")", true);
	if (!Result.bSuccess)
	{
		LogExecutionError("StartServer", Result);
		return false;
	}

	bServerStarted = true;
	ServerPort = Port;
	return true;
}

void MCPPlugin::StopServer()
{
	if (!bServerStarted)
		return;

	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::StopServer: MCP server is not stopped, because it should be stopped on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	// MCPPlugin depends on PythonPlugin, so PythonPlugin is still loaded when MCPPlugin is unloaded.
	PythonPluginInterface* Python = FindPython();
	if (Python != nullptr)
	{
		const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.stop(timeout=3)", true);
		if (!Result.bSuccess)
			LogExecutionError("StopServer", Result);
	}

	bServerStarted = false;
	ServerPort = 0;
}

bool MCPPlugin::IsServerRunning()
{
	if (!bServerStarted || !IsMainThread())
		return false;

	PythonPluginInterface* Python = FindPython();
	if (Python == nullptr)
		return false;

	// Server thread could have stopped because of an error.
	const PythonPluginInterface::ExecutionResult Result = Python->Evaluate("__import__('focalengine_mcp').is_running()");
	return Result.bSuccess && Result.Value == "True";
}

std::string MCPPlugin::GetServerURL()
{
	if (!bServerStarted)
		return "";

	return "http://127.0.0.1:" + std::to_string(ServerPort) + "/mcp";
}

bool MCPPlugin::RegisterTool(const std::string& OwnerName, const std::string& Name, const std::string& Description, const std::string& InputJSONSchema, ToolHandler Handler)
{
	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because it should be registered on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (!IsValidName(Name, MaximumToolNameLength, false))
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because its name should have 1 to 46 characters from A-Z, a-z, 0-9, _ and -", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (!Handler)
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because its handler is empty", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	std::string SchemaError;
	if (!IsValidInputSchema(InputJSONSchema, SchemaError))
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because its input schema is not valid: " + SchemaError, "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	const auto ExistingTool = Tools->find(Name);
	if (ExistingTool != Tools->end())
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because \"" + ExistingTool->second.OwnerName + "\" already registered tool with that name", "MCP_PLUGIN", FE_LOG_WARNING);
		return false;
	}

	(*Tools)[Name] = RegisteredTool{ OwnerName, Description, InputJSONSchema, std::move(Handler) };
	return true;
}

bool MCPPlugin::UnregisterTool(const std::string& Name)
{
	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::UnregisterTool: Tool \"" + Name + "\" is not unregistered, because it should be unregistered on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	return Tools->erase(Name) > 0;
}

void MCPPlugin::UnregisterToolsOfOwner(const std::string& OwnerName)
{
	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::UnregisterToolsOfOwner: Tools of \"" + OwnerName + "\" are not unregistered, because they should be unregistered on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	auto Iterator = Tools->begin();
	while (Iterator != Tools->end())
	{
		if (Iterator->second.OwnerName == OwnerName)
		{
			Iterator = Tools->erase(Iterator);
		}
		else
		{
			Iterator++;
		}
	}
}