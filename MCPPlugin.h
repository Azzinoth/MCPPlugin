#include "SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.h"
#include "Public/MCPPluginInterface.h"
using namespace FocalEngine;

class MCPPlugin : public FEPluginCore, public MCPPluginInterface
{
public:
	void OnLoad() override;
	void OnUnload() override;
	void OnUpdate(double DeltaTime) override;
};