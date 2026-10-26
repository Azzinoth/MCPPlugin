#pragma once
#include "SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.h"
// Copy of PythonPlugin public header, MCP server runs in Python that PythonPlugin provides.
#include "SubSystems/PythonPlugin/PythonPluginInterface.h"
#include "Public/MCPPluginInterface.h"
using namespace FocalEngine;

class MCPPlugin : public FEPluginCore, public MCPPluginInterface
{
public:
	void OnLoad() override;
	void OnUnload() override;
private:
	bool bServerStarted = false;
};