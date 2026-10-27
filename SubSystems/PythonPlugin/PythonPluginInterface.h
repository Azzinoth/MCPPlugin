#pragma once
#include <functional>
#include <string>

// Public API of PythonPlugin, applications include only this header.
// Usage:
//	using PluginInterface = PythonPluginInterface;
//	PluginInterface* Plugin = PLUGIN_MANAGER.GetPlugin<PluginInterface>(UNIQUE_ID.FromString(PluginInterface::PluginID));
//	if (Plugin != nullptr && Plugin->GetInterfaceVersion() >= PluginInterface::InterfaceVersion)
//		Plugin->...;
// Functions are only appended, so plugin with newer version works with code that was built with older header.
class PythonPluginInterface
{
public:
	// Same as "ID" in PythonPlugin.json.
	static constexpr const char* PluginID = "a1262d03-a32d-4dbe-aa78-faf2e862ce72";

	// Increase when anything in this file changes.
	static constexpr int InterfaceVersion = 2;

	struct ExecutionResult
	{
		bool bSuccess = false;
		// Everything that code printed to stdout and stderr, in the order it was printed.
		std::string Output;
		// repr() of the last line if it is an expression with value other than None, like in interactive Python.
		std::string Value;
		// Traceback if code raised an exception.
		std::string Error;
	};

	// Order of virtual functions is part of the interface, new functions should be added only at the end.
	virtual ~PythonPluginInterface() = default;
	virtual int GetInterfaceVersion() const { return InterfaceVersion; }

	// False if Python failed to start, then other functions do nothing.
	virtual bool IsInitialized() const = 0;
	virtual std::string GetPythonVersion() const = 0;

	// Should be called on main thread, engine is available to Python only there.
	// Code runs in __main__ namespace that keeps variables between calls, or in new namespace with bIsolatedNamespace.
	virtual ExecutionResult RunString(const std::string& Code, bool bIsolatedNamespace = false) = 0;
	virtual ExecutionResult RunFile(const std::string& FilePath, bool bIsolatedNamespace = false) = 0;
	// Expression is evaluated in __main__ namespace, its repr() is in Value.
	virtual ExecutionResult Evaluate(const std::string& Expression) = 0;
	// Removes all variables from __main__ namespace.
	virtual void ResetNamespace() = 0;

	virtual bool AddModuleSearchPath(const std::string& DirectoryPath) = 0;
	// Checks that module could be imported, without importing it.
	virtual bool IsModuleAvailable(const std::string& ModuleName) = 0;

	// Version 2.
	// Lets other plugins and applications expose C++ functions to Python without linking Python,
	// Python calls them with focalengine.call_native_function(name, arguments_json).
	// Function gets and returns JSON text and runs on main thread, exception that it throws is raised in Python as RuntimeError.
	using NativeFunction = std::function<std::string(const std::string& ArgumentsJSON)>;
	// Main thread only, returns false if function with that name is already registered.
	virtual bool RegisterNativeFunction(const std::string& OwnerName, const std::string& FunctionName, NativeFunction Function) = 0;
	// Main thread only, module should call it before it is unloaded, because Python would call into unloaded code otherwise.
	virtual void UnregisterNativeFunctions(const std::string& OwnerName) = 0;
};
