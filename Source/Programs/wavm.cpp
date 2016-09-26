#include "Core/Core.h"
#include "Core/Serialization.h"
#include "Core/Platform.h"
#include "WAST/WAST.h"
#include "Runtime/Runtime.h"
#include "Runtime/Linker.h"
#include "Runtime/Intrinsics.h"
#include "Emscripten/Emscripten.h"
#include "WebAssembly/Module.h"
#include "WebAssembly/Operations.h"

#include "CLI.h"

using namespace WebAssembly;
using namespace Runtime;

void showHelp()
{
	std::cerr << "Usage: wavm [switches] [programfile] [--] [arguments]" << std::endl;
	std::cerr << "  in.wast|in.wasm\t\tSpecify program file (.wast/.wasm)" << std::endl;
	std::cerr << "  -f|--function name\t\tSpecify function name to run in module rather than main" << std::endl;
	std::cerr << "  -c|--check\t\t\tExit after checking that the program is valid" << std::endl;
	std::cerr << "  -d|--debug\t\t\tWrite additional debug information to stdout" << std::endl;
	std::cerr << "  --\t\t\t\tStop parsing arguments" << std::endl;
}

struct RootResolver : Resolver
{
	std::map<std::string,Resolver*> moduleNameToResolverMap;

	bool resolve(const char* moduleName,const char* exportName,ObjectType type,Object*& outObject) override
	{
		// Try to resolve an intrinsic first.
		if(IntrinsicResolver::singleton.resolve(moduleName,exportName,type,outObject)) { return true; }

		// Then look for a named module.
		auto namedResolverIt = moduleNameToResolverMap.find(moduleName);
		if(namedResolverIt != moduleNameToResolverMap.end())
		{
			return namedResolverIt->second->resolve(moduleName,exportName,type,outObject);
		}

		// Finally, stub in missing function imports.
		if(type.kind == ObjectKind::function)
		{
			// Generate a function body that just uses the unreachable op to fault if called.
			ArrayOutputStream codeStream;
			OperationEncoder encoder(codeStream);
			encoder.unreachable();
			encoder.end();

			// Generate a module for the stub function.
			Module stubModule;
			DisassemblyNames stubModuleNames;
			stubModule.code = codeStream.getBytes();
			stubModule.types.push_back(type.function);
			stubModule.functionDefs.push_back({{},0,{0,stubModule.code.size()}});
			stubModule.exports.push_back({"importStub",ObjectKind::function,0});
			stubModuleNames.functions.push_back(std::string(moduleName) + "." + exportName);
			setDisassemblyNames(stubModule,stubModuleNames);
			WebAssembly::validate(stubModule);

			// Instantiate the module and return the stub function instance.
			auto stubModuleInstance = instantiateModule(stubModule,{});
			outObject = getInstanceExport(stubModuleInstance,"importStub");
			Log::printf(Log::Category::error,"Generated stub for missing function import %s.%s : %s\n",moduleName,exportName,asString(type).c_str());
			return true;
		}

		return false;
	}
};

int mainBody(const char* filename,const char* functionName,bool onlyCheck,char** args)
{
	Module module;
	if(filename)
	{
		if(!loadModule(filename,module)) { return EXIT_FAILURE; }
	}
	else
	{
		showHelp();
		return EXIT_FAILURE;
	}

	if(onlyCheck) { return EXIT_SUCCESS; }

	// Link and instantiate the module.
	RootResolver rootResolver;
	ModuleInstance* moduleInstance = linkAndInstantiateModule(module,rootResolver);
	if(!moduleInstance) { return EXIT_FAILURE; }
	Emscripten::initInstance(module,moduleInstance);

	// Look up the function export to call.
	FunctionInstance* functionInstance;
	if(!functionName)
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"main"));
		if(!functionInstance) { functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"_main")); }
		if(!functionInstance)
		{
			std::cerr << "Module does not export main function" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,functionName));
		if(!functionInstance)
		{
			std::cerr << "Module does not export '" << functionName << "'" << std::endl;
			return EXIT_FAILURE;
		}
	}
	const FunctionType* functionType = getFunctionType(functionInstance);

	// Set up the arguments for the invoke.
	std::vector<Value> invokeArgs;
	if(!functionName)
	{
		if(functionType->parameters.size() == 2)
		{
			Memory* defaultMemory = Runtime::getDefaultMemory(moduleInstance);
			if(!defaultMemory)
			{
				std::cerr << "Module does not declare a default memory object to put arguments in." << std::endl;
				return EXIT_FAILURE;
			}

			std::vector<const char*> argStrings;
			argStrings.push_back(filename);
			while(*args) { argStrings.push_back(*args++); };

			Emscripten::injectCommandArgs(argStrings,invokeArgs);
		}
		else if(functionType->parameters.size() > 0)
		{
			std::cerr << "WebAssembly function requires " << functionType->parameters.size() << " argument(s), but only 0 or 2 can be passed!" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		for(uint32 i = 0; args[i]; ++i)
		{
			Value value;
			switch(functionType->parameters[i])
			{
			case ValueType::i32: value = (uint32)atoi(args[i]); break;
			case ValueType::i64: value = (uint64)atol(args[i]); break;
			case ValueType::f32: value = (float32)atof(args[i]); break;
			case ValueType::f64: value = atof(args[i]); break;
			default: Core::unreachable();
			}
			invokeArgs.push_back(value);
		}
	}

	// Invoke the function.
	Core::Timer executionTimer;
	auto functionResult = invokeFunction(functionInstance,invokeArgs);
	Log::logTimer("Invoked function",executionTimer);

	if(functionName)
	{
		Log::printf(Log::Category::debug,"%s returned: %s\n",functionName,asString(functionResult).c_str());
		return EXIT_SUCCESS;
	}
	else if(functionResult.type == ResultType::i32) { return functionResult.i32; }
	else { return EXIT_SUCCESS; }
}

int commandMain(int argc,char** argv)
{
	const char* filename = nullptr;
	const char* functionName = nullptr;

	bool onlyCheck = false;
	auto args = argv;
	while(*++args)
	{
		if(!strcmp(*args, "--function") || !strcmp(*args, "-f"))
		{
			if(!*++args) { showHelp(); return EXIT_FAILURE; }
			functionName = *args;
		}
		else if(!strcmp(*args, "--check") || !strcmp(*args, "-c"))
		{
			onlyCheck = true;
		}
		else if(!strcmp(*args, "--debug") || !strcmp(*args, "-d"))
		{
			Log::setCategoryEnabled(Log::Category::debug,true);
		}
		else if(!strcmp(*args, "--"))
		{
			++args;
			break;
		}
		else if(!strcmp(*args, "--help") || !strcmp(*args, "-h"))
		{
			showHelp();
			return EXIT_SUCCESS;
		}
		else if(!filename)
		{
			filename = *args;
		}
		else { break; }
	}

	Runtime::init();

	int returnCode = EXIT_FAILURE;
	#ifdef __AFL_LOOP
	while(__AFL_LOOP(2000))
	#endif
	{
		returnCode = mainBody(filename,functionName,onlyCheck,args);
		Runtime::freeUnreferencedObjects({});
	}
	return returnCode;
}
