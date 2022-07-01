#include "stdafx.h"

#ifndef LIBRETRO
#include "Lua/lua.hpp"
#include "Lua/luasocket.hpp"
#include "Debugger/LuaScriptingContext.h"
#include "Debugger/LuaApi.h"
#include "Debugger/LuaCallHelper.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "EventType.h"

LuaScriptingContext* LuaScriptingContext::_context = nullptr;

LuaScriptingContext::LuaScriptingContext(Debugger* debugger) : ScriptingContext(debugger)
{
	_settings = debugger->GetEmulator()->GetSettings();
}

LuaScriptingContext::~LuaScriptingContext()
{
	if(_lua) {
		//Cleanup all references, this is required to prevent crashes that can occur when calling lua_close
		std::unordered_set<int> references;
		for(int i = (int)CallbackType::CpuRead; i <= (int)CallbackType::CpuExec; i++) {
			for(MemoryCallback& callback : _callbacks[i]) {
				references.emplace(callback.Reference);
			}
		}

		for(int i = (int)EventType::Reset; i < (int)EventType::EventTypeSize; i++) {
			for(int& ref : _eventCallbacks[i]) {
				references.emplace(ref);
			}
		}

		for(const int& ref : references) {
			luaL_unref(_lua, LUA_REGISTRYINDEX, ref);
		}

		lua_close(_lua);
		_lua = nullptr;
	}
}

void LuaScriptingContext::ExecutionCountHook(lua_State *lua, lua_Debug *ar)
{
	uint32_t timeout = _context->_settings->GetDebugConfig().ScriptTimeout;
	if(_context->_timer.GetElapsedMS() > timeout * 1000) {
		luaL_error(lua, (std::string("Maximum execution time (") + std::to_string(timeout) + " seconds) exceeded.").c_str());
	}
}

void LuaScriptingContext::LuaOpenLibs(lua_State* L, bool allowIoOsAccess)
{
	constexpr luaL_Reg loadedlibs[] = {
	  {"_G", luaopen_base},
	  {LUA_LOADLIBNAME, luaopen_package},
	  {LUA_COLIBNAME, luaopen_coroutine},
	  {LUA_TABLIBNAME, luaopen_table},
	  {LUA_IOLIBNAME, luaopen_io},
	  {LUA_OSLIBNAME, luaopen_os},
	  {LUA_STRLIBNAME, luaopen_string},
	  {LUA_MATHLIBNAME, luaopen_math},
	  {LUA_UTF8LIBNAME, luaopen_utf8},
	  {LUA_DBLIBNAME, luaopen_debug},
	  {NULL, NULL}
	};

	const luaL_Reg* lib;
	/* "require" functions from 'loadedlibs' and set results to global table */
	for(lib = loadedlibs; lib->func; lib++) {
		if(!allowIoOsAccess) {
			//Skip loading IO, OS and Package lib when sandboxed
			if(strcmp(lib->name, LUA_IOLIBNAME) == 0 || strcmp(lib->name, LUA_OSLIBNAME) == 0 || strcmp(lib->name, LUA_LOADLIBNAME) == 0) {
				continue;
			}
		}
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);  /* remove lib */
	}
}

bool LuaScriptingContext::LoadScript(string scriptName, string scriptContent, Debugger* debugger)
{
	_scriptName = scriptName;

	int iErr = 0;
	_lua = luaL_newstate();
	
	_context = this;
	LuaApi::SetContext(this);

	EmuSettings* settings = debugger->GetEmulator()->GetSettings();
	bool allowIoOsAccess = settings->GetDebugConfig().ScriptAllowIoOsAccess;
	LuaOpenLibs(_lua, allowIoOsAccess);
	
	//Prevent lua code from loading any files
	SANDBOX_ALLOW_LOADFILE = allowIoOsAccess ? 1 : 0;

	//Load LuaSocket into Lua core
	if(allowIoOsAccess && settings->GetDebugConfig().ScriptAllowNetworkAccess) {
		lua_getglobal(_lua, "package");
		lua_getfield(_lua, -1, "preload");
		lua_pushcfunction(_lua, luaopen_socket_core);
		lua_setfield(_lua, -2, "socket.core");
		lua_pushcfunction(_lua, luaopen_mime_core);
		lua_setfield(_lua, -2, "mime.core");
		lua_pop(_lua, 2);
	}

	luaL_requiref(_lua, "emu", LuaApi::GetLibrary, 1);
	Log("Loading script...");
	if((iErr = luaL_loadbufferx(_lua, scriptContent.c_str(), scriptContent.size(), ("@" + scriptName).c_str(), nullptr)) == 0) {
		_timer.Reset();
		lua_sethook(_lua, LuaScriptingContext::ExecutionCountHook, LUA_MASKCOUNT, 1000);
		if((iErr = lua_pcall(_lua, 0, LUA_MULTRET, 0)) == 0) {
			//Script loaded properly
			Log("Script loaded successfully.");
			_initDone = true;
			return true;
		}
	}

	if(lua_isstring(_lua, -1)) {
		Log(lua_tostring(_lua, -1));
	}
	return false;
}

void LuaScriptingContext::UnregisterMemoryCallback(CallbackType type, int startAddr, int endAddr, CpuType cpuType, int reference)
{
	ScriptingContext::UnregisterMemoryCallback(type, startAddr, endAddr, cpuType, reference);
	luaL_unref(_lua, LUA_REGISTRYINDEX, reference);
}

void LuaScriptingContext::UnregisterEventCallback(EventType type, int reference)
{
	ScriptingContext::UnregisterEventCallback(type, reference);
	luaL_unref(_lua, LUA_REGISTRYINDEX, reference);
}

void LuaScriptingContext::InternalCallMemoryCallback(uint32_t addr, uint8_t &value, CallbackType type, CpuType cpuType)
{
	if(_callbacks[(int)type].empty()) {
		return;
	}

	_timer.Reset();
	_context = this;
	lua_sethook(_lua, LuaScriptingContext::ExecutionCountHook, LUA_MASKCOUNT, 1000); 
	LuaApi::SetContext(this);
	for(MemoryCallback &callback: _callbacks[(int)type]) {
		if(callback.Type != cpuType || addr < callback.StartAddress || addr > callback.EndAddress) {
			continue;
		}

		int top = lua_gettop(_lua);
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, callback.Reference);
		lua_pushinteger(_lua, addr);
		lua_pushinteger(_lua, value);
		if(lua_pcall(_lua, 2, LUA_MULTRET, 0) != 0) {
			Log(lua_tostring(_lua, -1));
		} else {
			int returnParamCount = lua_gettop(_lua) - top;
			if(returnParamCount && lua_isinteger(_lua, -1)) {
				int newValue = (int)lua_tointeger(_lua, -1);
				value = (uint8_t)newValue;
			}
			lua_settop(_lua, top);
		}
	}
}

int LuaScriptingContext::InternalCallEventCallback(EventType type)
{
	if(_eventCallbacks[(int)type].empty()) {
		return 0;
	}

	_timer.Reset();
	_context = this;
	lua_sethook(_lua, LuaScriptingContext::ExecutionCountHook, LUA_MASKCOUNT, 1000); 
	LuaApi::SetContext(this);
	LuaCallHelper l(_lua);
	for(int &ref : _eventCallbacks[(int)type]) {
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		if(lua_pcall(_lua, 0, 0, 0) != 0) {
			Log(lua_tostring(_lua, -1));
		}
	}
	return l.ReturnCount();
}
#endif