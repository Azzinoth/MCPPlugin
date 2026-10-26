#include "MCPPlugin.h"
#include <cstdlib>

// DO NOT CHANGE THIS LINE.
// The plugin manager uses it to create and destroy the plugin instance.
REGISTER_PLUGIN(MCPPlugin)

namespace
{
	const int DefaultPort = 5187;

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
}

void MCPPlugin::OnLoad()
{
	// Applications that link plugin statically, for example built games, decide themselves whether server starts, so they do not open a port by default.
#ifdef FOCAL_ENGINE_SHARED
	PythonPluginInterface* Python = PLUGIN_MANAGER.GetPlugin<PythonPluginInterface>(UNIQUE_ID.FromString(PythonPluginInterface::PluginID));
	if (Python == nullptr)
	{
		LOG.Add("MCPPlugin::OnLoad: MCP server is not started, because PythonPlugin is not loaded", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	if (Python->GetInterfaceVersion() < PythonPluginInterface::InterfaceVersion)
	{
		LOG.Add("MCPPlugin::OnLoad: MCP server is not started, because PythonPlugin interface version is " + std::to_string(Python->GetInterfaceVersion()) +
				", at least " + std::to_string(PythonPluginInterface::InterfaceVersion) + " is needed", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	if (!Python->IsInitialized())
	{
		LOG.Add("MCPPlugin::OnLoad: MCP server is not started, because Python failed to start, PYTHON_PLUGIN log has the reason", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	// Binary modules of vendored packages are built only for this Python version.
	if (Python->GetPythonVersion().rfind("3.13.", 0) != 0)
	{
		LOG.Add("MCPPlugin::OnLoad: MCP server is not started, because its packages are built for Python 3.13, but PythonPlugin has Python " + Python->GetPythonVersion(), "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	const std::string PythonDirectory = GetPluginDirectory() + "Data/Python";
	if (!Python->AddModuleSearchPath(PythonDirectory))
	{
		LOG.Add("MCPPlugin::OnLoad: MCP server is not started, because \"" + PythonDirectory + "\" could not be added to Python module search path", "MCP_PLUGIN", FE_LOG_ERROR);
		return;
	}

	// Returns right away, server reports to log when it listens.
	const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.start(port=" + std::to_string(GetPort()) + ")", true);
	if (!Result.bSuccess)
	{
		LogExecutionError("OnLoad", Result);
		return;
	}

	bServerStarted = true;
#endif
}

void MCPPlugin::OnUnload()
{
	if (!bServerStarted)
		return;

	// MCPPlugin depends on PythonPlugin, so PythonPlugin is unloaded after it.
	PythonPluginInterface* Python = PLUGIN_MANAGER.GetPlugin<PythonPluginInterface>(UNIQUE_ID.FromString(PythonPluginInterface::PluginID));
	if (Python == nullptr || !Python->IsInitialized())
		return;

	const PythonPluginInterface::ExecutionResult Result = Python->RunString("import focalengine_mcp\nfocalengine_mcp.stop(timeout=3)", true);
	if (!Result.bSuccess)
		LogExecutionError("OnUnload", Result);

	bServerStarted = false;
}