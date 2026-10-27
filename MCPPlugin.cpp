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
	const std::string NativeFunctionOwner = "MCPPlugin";

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

	bool ParseJSON(const std::string& Text, Json::Value& OutValue, std::string& OutError)
	{
		Json::CharReaderBuilder Builder;
		const std::unique_ptr<Json::CharReader> Reader(Builder.newCharReader());
		return Reader->parse(Text.c_str(), Text.c_str() + Text.size(), &OutValue, &OutError);
	}

	std::string ToCompactJSON(const Json::Value& Value)
	{
		Json::StreamWriterBuilder Builder;
		Builder["indentation"] = "";
		return Json::writeString(Builder, Value);
	}

	std::string EncodeBase64(const std::vector<unsigned char>& Data)
	{
		static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Result;
		Result.reserve((Data.size() + 2) / 3 * 4);
		for (size_t i = 0; i < Data.size(); i += 3)
		{
			const unsigned int Block = (Data[i] << 16) | ((i + 1 < Data.size() ? Data[i + 1] : 0) << 8) | (i + 2 < Data.size() ? Data[i + 2] : 0);
			Result += Alphabet[(Block >> 18) & 63];
			Result += Alphabet[(Block >> 12) & 63];
			Result += i + 1 < Data.size() ? Alphabet[(Block >> 6) & 63] : '=';
			Result += i + 2 < Data.size() ? Alphabet[Block & 63] : '=';
		}

		return Result;
	}

	MCPPluginInterface::ToolResult MakeErrorResult(const std::string& Text)
	{
		MCPPluginInterface::ToolResult Result;
		Result.bIsError = true;
		Result.Text = Text;
		return Result;
	}

	std::string ToolResultToJSON(const MCPPluginInterface::ToolResult& Result)
	{
		Json::Value Output(Json::objectValue);
		Output["is_error"] = Result.bIsError;
		Output["text"] = Result.Text;
		if (!Result.ImageData.empty())
		{
			Output["image_base64"] = EncodeBase64(Result.ImageData);
			Output["image_mime_type"] = Result.ImageMIMEType.empty() ? std::string("image/png") : Result.ImageMIMEType;
		}

		return ToCompactJSON(Output);
	}

	// Clients reject whole tool list when one schema breaks their rules, so tool with such schema is not registered.
	bool ParseInputSchema(const std::string& InputJSONSchema, Json::Value& OutSchema, std::string& OutError)
	{
		Json::Value Root;
		std::string ParseErrors;
		if (!ParseJSON(InputJSONSchema, Root, ParseErrors))
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

		if (Root.isMember("properties"))
		{
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
		}

		OutSchema = Root;
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

	if (bNativeFunctionsRegistered)
	{
		// PythonPlugin is unloaded after MCPPlugin, so it should not keep functions that point into this module.
		PythonPluginInterface* Python = PLUGIN_MANAGER.GetPlugin<PythonPluginInterface>(UNIQUE_ID.FromString(PythonPluginInterface::PluginID));
		if (Python != nullptr)
			Python->UnregisterNativeFunctions(NativeFunctionOwner);

		bNativeFunctionsRegistered = false;
	}

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

void MCPPlugin::OnUpdate(double DeltaTime)
{
	// Reload waits for next update, so many registrations cause one reload,
	// and RegisterTool that is called from a tool handler does not call back into Python while handler runs.
	if (!bToolsChanged || !bServerStarted || !bNativeFunctionsRegistered)
		return;

	bToolsChanged = false;
	PythonPluginInterface* Python = FindPython();
	if (Python == nullptr)
		return;

	const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.reload_native_tools()", true);
	if (!Result.bSuccess)
		LogExecutionError("OnUpdate", Result);
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

	RegisterNativeFunctions(Python);

	// Returns right away, server reports to log when it listens. Tools registered from C++ are read during start.
	const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.start(port=" + std::to_string(Port) + ")", true);
	if (!Result.bSuccess)
	{
		LogExecutionError("StartServer", Result);
		return false;
	}

	bServerStarted = true;
	ServerPort = Port;
	bToolsChanged = false;
	return true;
}

void MCPPlugin::RegisterNativeFunctions(PythonPluginInterface* Python)
{
	if (bNativeFunctionsRegistered)
		return;

	if (Python->GetInterfaceVersion() < 2)
	{
		LOG.Add("MCPPlugin::RegisterNativeFunctions: Tools registered with RegisterTool are not served, because they need PythonPlugin interface version 2, PythonPlugin has version " +
				std::to_string(Python->GetInterfaceVersion()), "MCP_PLUGIN", FE_LOG_WARNING);
		return;
	}

	bNativeFunctionsRegistered =
		Python->RegisterNativeFunction(NativeFunctionOwner, "MCPPlugin.ListTools", [this](const std::string&) { return ListToolsAsJSON(); }) &&
		Python->RegisterNativeFunction(NativeFunctionOwner, "MCPPlugin.CallTool", [this](const std::string& RequestJSON) { return CallToolFromJSON(RequestJSON); }) &&
		Python->RegisterNativeFunction(NativeFunctionOwner, "MCPPlugin.SetReservedToolNames", [this](const std::string& NamesJSON) {
			SetReservedToolNames(NamesJSON);
			return std::string("{}");
		});

	if (!bNativeFunctionsRegistered)
		Python->UnregisterNativeFunctions(NativeFunctionOwner);
}

std::string MCPPlugin::ListToolsAsJSON() const
{
	Json::Value Result(Json::arrayValue);
	for (const auto& Tool : *Tools)
	{
		Json::Value Entry(Json::objectValue);
		Entry["name"] = Tool.first;
		Entry["description"] = Tool.second.Description;
		Entry["input_schema"] = Tool.second.InputSchema;
		Result.append(Entry);
	}

	return ToCompactJSON(Result);
}

std::string MCPPlugin::CallToolFromJSON(const std::string& RequestJSON)
{
	Json::Value Request;
	std::string ParseError;
	if (!ParseJSON(RequestJSON, Request, ParseError) || !Request.isObject() || !Request.isMember("name") || !Request["name"].isString())
		return ToolResultToJSON(MakeErrorResult("Request should be JSON object with \"name\" and \"arguments_json\""));

	const std::string Name = Request["name"].asString();
	const auto Tool = Tools->find(Name);
	if (Tool == Tools->end())
		return ToolResultToJSON(MakeErrorResult("Tool \"" + Name + "\" is not registered anymore"));

	// Copy keeps handler alive even if it unregisters its tool while it runs.
	const ToolHandler Handler = Tool->second.Handler;
	const std::string ArgumentsJSON = Request.isMember("arguments_json") && Request["arguments_json"].isString() ? Request["arguments_json"].asString() : std::string("{}");
	try
	{
		return ToolResultToJSON(Handler(ArgumentsJSON));
	}
	catch (const std::exception& Exception)
	{
		return ToolResultToJSON(MakeErrorResult("Tool \"" + Name + "\" failed: " + Exception.what()));
	}
	catch (...)
	{
		return ToolResultToJSON(MakeErrorResult("Tool \"" + Name + "\" failed with unknown exception"));
	}
}

void MCPPlugin::SetReservedToolNames(const std::string& NamesJSON)
{
	Json::Value Names;
	std::string ParseError;
	if (!ParseJSON(NamesJSON, Names, ParseError) || !Names.isArray())
		return;

	ReservedToolNames.clear();
	for (Json::ArrayIndex i = 0; i < Names.size(); i++)
	{
		if (Names[i].isString())
			ReservedToolNames.insert(Names[i].asString());
	}

	// Tools registered before server started first time could not be checked against these names.
	auto Iterator = Tools->begin();
	while (Iterator != Tools->end())
	{
		if (ReservedToolNames.count(Iterator->first) > 0)
		{
			LOG.Add("MCPPlugin: Tool \"" + Iterator->first + "\" of \"" + Iterator->second.OwnerName + "\" is unregistered, because built-in tool of MCP server has that name", "MCP_PLUGIN", FE_LOG_ERROR);
			Iterator = Tools->erase(Iterator);
		}
		else
		{
			Iterator++;
		}
	}
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

	if (ReservedToolNames.count(Name) > 0)
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because built-in tool of MCP server has that name", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (!Handler)
	{
		LOG.Add("MCPPlugin::RegisterTool: Tool \"" + Name + "\" of \"" + OwnerName + "\" is not registered, because its handler is empty", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	Json::Value InputSchema;
	std::string SchemaError;
	if (!ParseInputSchema(InputJSONSchema, InputSchema, SchemaError))
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

	(*Tools)[Name] = RegisteredTool{ OwnerName, Description, InputSchema, std::move(Handler) };
	bToolsChanged = true;
	return true;
}

bool MCPPlugin::UnregisterTool(const std::string& Name)
{
	if (!IsMainThread())
	{
		LOG.Add("MCPPlugin::UnregisterTool: Tool \"" + Name + "\" is not unregistered, because it should be unregistered on main thread", "MCP_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	if (Tools->erase(Name) == 0)
		return false;

	bToolsChanged = true;
	return true;
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
			bToolsChanged = true;
		}
		else
		{
			Iterator++;
		}
	}
}